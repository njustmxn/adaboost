#include <iostream>
#include <fstream> 
#include <cmath>
#include <vector>
#include <typeinfo>
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/contrib/contrib.hpp"

#include "Pyramid.h"
#include "sse.hpp"
#include "wrappers.hpp"

using namespace std;
using namespace cv;

// helper for gradHist, quantize O and M into O0, O1 and M0, M1 (uses sse)
void gradQuantize( const float *O, const float *M, int *O0, int *O1, float *M0, float *M1,
  int nb, int n, float norm, int nOrients, bool full, bool interpolate )
{
  // assumes all *OUTPUT* matrices are 4-byte aligned
  int i, o0, o1; float o, od, m;
  __m128i _o0, _o1, *_O0, *_O1; __m128 _o, _od, _m, *_M0, *_M1;
  // define useful constants
  const float oMult=(float)nOrients/(full?2*(float)CV_PI:(float)CV_PI); const int oMax=nOrients*nb;
  const __m128 _norm=SET(norm), _oMult=SET(oMult), _nbf=SET((float)nb);
  const __m128i _oMax=SET(oMax), _nb=SET(nb);
  // perform the majority of the work with sse
  _O0=(__m128i*) O0; _O1=(__m128i*) O1; _M0=(__m128*) M0; _M1=(__m128*) M1;
  if( interpolate ) for( i=0; i<=n-4; i+=4 ) {
    _o=MUL(LDu(O[i]),_oMult); _o0=CVT(_o); _od=SUB(_o,CVT(_o0));
    _o0=CVT(MUL(CVT(_o0),_nbf)); _o0=AND(CMPGT(_oMax,_o0),_o0); *_O0++=_o0;
    _o1=ADD(_o0,_nb); _o1=AND(CMPGT(_oMax,_o1),_o1); *_O1++=_o1;
    _m=MUL(LDu(M[i]),_norm); *_M1=MUL(_od,_m); *_M0++=SUB(_m,*_M1); _M1++;
  } else for( i=0; i<=n-4; i+=4 ) {
    _o=MUL(LDu(O[i]),_oMult); _o0=CVT(ADD(_o,SET(.5f)));
    _o0=CVT(MUL(CVT(_o0),_nbf)); _o0=AND(CMPGT(_oMax,_o0),_o0); *_O0++=_o0;
    *_M0++=MUL(LDu(M[i]),_norm); *_M1++=SET(0.f); *_O1++=SET(0);
  }
  // compute trailing locations without sse
  if( interpolate ) for(; i<n; i++ ) {
    o=O[i]*oMult; o0=(int) o; od=o-o0;
    o0*=nb; if(o0>=oMax) o0=0; O0[i]=o0;
    o1=o0+nb; if(o1==oMax) o1=0; O1[i]=o1;
    m=M[i]*norm; M1[i]=od*m; M0[i]=m-M1[i];
  } else for(; i<n; i++ ) {
    o=O[i]*oMult; o0=(int) (o+.5f);
    o0*=nb; if(o0>=oMax) o0=0; O0[i]=o0;
    M0[i]=M[i]*norm; M1[i]=0; O1[i]=0;
  }
}

// compute nOrients gradient histograms per bin x bin block of pixels
void gradHist( const float *M,const float *O, float *H, int h, int w, int bin, int nOrients, int softBin, bool full )
{
  const int hb=h/bin, wb=w/bin, h0=hb*bin, w0=wb*bin, nb=wb*hb;
  const float s=(float)bin, sInv=1/s, sInv2=1/s/s;
  float *H0, *H1, *M0, *M1; int x, y; int *O0, *O1; float xb, init;

  O0=(int*)alMalloc(w*sizeof(int),16); M0=(float*) alMalloc(w*sizeof(float),16);
  O1=(int*)alMalloc(w*sizeof(int),16); M1=(float*) alMalloc(w*sizeof(float),16);

  // main loop
  for( x=0; x<h0; x++ ) 
  {
    // compute target orientation bins for entire column - very fast
    gradQuantize(O+x*w,M+x*w,O0,O1,M0,M1,nb,w0,sInv2,nOrients,full,softBin>=0);
    if( softBin<0 && softBin%2==0 ) {
      // no interpolation w.r.t. either orienation or spatial bin
      H1=H+(x/bin)*wb;
      #define GH H1[O0[y]]+=M0[y]; y++;
      if( bin==1 )      for(y=0; y<w0;) { GH; H1++; }
      else if( bin==2 ) for(y=0; y<w0;) { GH; GH; H1++; }
      else if( bin==3 ) for(y=0; y<w0;) { GH; GH; GH; H1++; }
      else if( bin==4 ) for(y=0; y<w0;) { GH; GH; GH; GH; H1++; }
      else for( y=0; y<w0;) { for( int y1=0; y1<bin; y1++ ) { GH; } H1++; }
      #undef GH

    } else if( softBin%2==0 || bin==1 ) {
      // interpolate w.r.t. orientation only, not spatial bin
      H1=H+(x/bin)*wb;
      #define GH H1[O0[y]]+=M0[y]; H1[O1[y]]+=M1[y]; y++;
      if( bin==1 )      for(y=0; y<w0;) { GH; H1++; }
      else if( bin==2 ) for(y=0; y<w0;) { GH; GH; H1++; }
      else if( bin==3 ) for(y=0; y<w0;) { GH; GH; GH; H1++; }
      else if( bin==4 ) for(y=0; y<w0;) { GH; GH; GH; GH; H1++; }
      else for( y=0; y<w0;) { for( int y1=0; y1<bin; y1++ ) { GH; } H1++; }
      #undef GH
    }   
  }
  alFree(O0); alFree(O1); alFree(M0); alFree(M1);
  // normalize boundary bins which only get 7/8 of weight of interior bins
  if( softBin%2!=0 ) for( int o=0; o<nOrients; o++ ) {
    x=0; for( y=0; y<hb; y++ ) H[o*nb+x+y*wb]*=8.f/7.f;
    y=0; for( x=0; x<wb; x++ ) H[o*nb+x+y*wb]*=8.f/7.f;
    x=wb-1; for( y=0; y<hb; y++ ) H[o*nb+x+y*wb]*=8.f/7.f;
    y=hb-1; for( x=0; x<wb; x++ ) H[o*nb+x+y*wb]*=8.f/7.f;
  }
}


 bool feature_Pyramids::conputeGradHist(  const Mat &mag,             //in : mag
                                          const Mat &ori,             //in : ori
                                          Mat &Ghist,           //out: g hist
                                          int binSize,          //in : number of bin
                                          int oritent,          //in : number of ori
                                          bool full     //in : ture->0-2pi, false->0-pi
                                          ) const
{
    if( mag.depth() !=CV_32F || ori.depth()!=CV_32F || mag.empty() || ori.empty() )
    {
        cout<<"mag, ori format wrong"<<endl;
        return false;
    }
    if( mag.cols!=ori.cols || mag.rows!=ori.rows)
    {
        cout<<"mag , ori size do not match "<<endl;
        return false;
    }
    
    Ghist = Mat::zeros( mag.rows/binSize*oritent, mag.cols/binSize, CV_32F );
    gradHist( (float*)mag.data, (float*)ori.data, (float*)Ghist.data, mag.rows, mag.cols, binSize, oritent, 0, full);

    return true;
}



// convolve one row of I by a [1 p 1] filter (uses SSE)
void convTri1Y( const float *I, float *O, int w, float p, int s ) {
  #define C4(m,o) ADD(ADD(LDu(I[m*j-1+o]),MUL(p,LDu(I[m*j+o]))),LDu(I[m*j+1+o]))
  int j=0, k=((~((size_t) O) + 1) & 15)/4, h2=(w-1)/2;
  if( s==2 ) 
  {
    for( ; j<k; j++ ) O[j]=I[2*j]+p*I[2*j+1]+I[2*j+2];
    for( ; j<h2-4; j+=4 ) STR(O[j],_mm_shuffle_ps(C4(2,1),C4(2,5),136));
    for( ; j<h2; j++ ) O[j]=I[2*j]+p*I[2*j+1]+I[2*j+2];
    if( w%2==0 ) O[j]=I[2*j]+(1+p)*I[2*j+1];
  } 
  else 
  {
    O[j]=(1+p)*I[j]+I[j+1]; j++; if(k==0) k=(w<=4) ? w-1 : 4;
    for( ; j<k; j++ ) O[j]=I[j-1]+p*I[j]+I[j+1];
    for( ; j<w-4; j+=4 ) STR(O[j],C4(1,0));
    for( ; j<w-1; j++ ) O[j]=I[j-1]+p*I[j]+I[j+1];
    O[j]=I[j-1]+(1+p)*I[j];
  }
  #undef C4
}

// convolve I by a [1 p 1] filter (uses SSE)
void convTri1( const float *I, float *O, int h, int w, int d, float p, int s=1) {
  const float nrm = 1.0f/((p+2)*(p+2)); int i, j, w0=w-(w%4);
  const float *Il, *Im, *Ir;
  float *T=(float*) alMalloc(w*sizeof(float),16);
  for( int d0=0; d0<d; d0++ ) 
      for( i=s/2; i<h; i+=s ) 
  {
    Il=Im=Ir=I+i*w+d0*h*w; if(i>0) Il-=w; if(i<h-1) Ir+=w;
    for( j=0; j<w0; j+=4 )
      STR(T[j],MUL(nrm,ADD(ADD(LDu(Il[j]),MUL(p,LDu(Im[j]))),LDu(Ir[j]))));
    for( j=w0; j<w; j++ ) T[j]=nrm*(Il[j]+p*Im[j]+Ir[j]);
    convTri1Y(T,O,w,p,s); O+=w/s;
  }
  alFree(T);
}


// compute x and y gradients for just one row (uses sse)
void grad1( const float *I,   //in :data
            float *Gx,  //out: gradient of x
            float *Gy,  //out: gradient of y
            int h,      //in : height
            int w,      //in : width
            int x )     //in : index of row
{
  int y, y1; 
  const float *Ip, *In; float r; __m128 *_Ip, *_In, *_G, _r;
  //compute row of Gy
  Ip=I-w; In=I+w; r=.5f;
  if(x==0) { r=1; Ip+=w; } else if(x==h-1) { r=1; In-=w; }      //on the border
  if( w<4 || w%4>0 || (size_t(I)&15) || (size_t(Gy)&15) ) {     //data align?
    for( int c=0; c<w; c++ ) *Gy++=(*In++-*Ip++)*r;
  } else {
    _G=(__m128*) Gy; _Ip=(__m128*) Ip; _In=(__m128*) In; _r = SET(r);
    for(int c=0; c<w; c+=4) *_G++=MUL(SUB(*_In++,*_Ip++),_r);
  }

  // compute row of Gx
  #define GRADX(r) *Gx++=(*In++-*Ip++)*r;
  Ip=I; In=Ip+1;
  // equivalent --> GRADX(1); Ip--; for(y=1; y<w-1; y++) GRADX(.5f); In--; GRADX(1);
  y1=((~((size_t) Gx) + 1) & 15)/4; if(y1==0) y1=4; if(y1>w-1) y1=w-1;      // y1 -> the number of element with out using sse
  GRADX(1); Ip--; for(y=1; y<y1; y++) GRADX(.5f);
  _r = SET(.5f); _G=(__m128*) Gx;
  for(; y+4<w-1; y+=4, Ip+=4, In+=4, Gx+=4)
    *_G++=MUL(SUB(LDu(*In),LDu(*Ip)),_r);
  for(; y<w-1; y++) GRADX(.5f); In--; GRADX(1);
  #undef GRADX
}

float* acosTable() {
  const int n=10000, b=10; int i;
  static float a[n*2+b*2]; static bool init=false;
  float *a1=a+n+b; if( init ) return a1;
  for( i=-n-b; i<-n; i++ )   a1[i]=(float)CV_PI;
  for( i=-n; i<n; i++ )      a1[i]=float(acos(i/float(n)));
  for( i=n; i<n+b; i++ )     a1[i]=0;
  for( i=-n-b; i<n/10; i++ ) if( a1[i] > (float)CV_PI-1e-6f ) a1[i]=(float)CV_PI-1e-6f;
  init=true; return a1;
}


// compute gradient magnitude and orientation at each location (uses sse)
void gradMag( float *I, float *M, float *O, int h, int w, int d, bool full ) {
  int x, y, y1, c, w4, s; float *Gx, *Gy, *M2; __m128 *_Gx, *_Gy, *_M2, _m;
  float *acost = acosTable(), acMult=10000.0f;
 
  // allocate memory for storing one row of output (padded so w4%4==0)
  w4=(w%4==0) ? w : w-(w%4)+4; s=d*w4*sizeof(float);
  M2=(float*) alMalloc(s,16); _M2=(__m128*) M2;
  Gx=(float*) alMalloc(s,16); _Gx=(__m128*) Gx;
  Gy=(float*) alMalloc(s,16); _Gy=(__m128*) Gy;

  // compute gradient magnitude and orientation for each column
  for( x=0; x<h; x++ ) 
  {
    // compute gradients (Gx, Gy) with maximum squared magnitude (M2)
    for(c=0; c<d; c++)          // compute for each channel, take the max value
    {
      grad1( I+x*w+c*w*h, Gx+c*w4, Gy+c*w4, h, w, x );
      for( y=0; y<w4/4; y++ ) 
      {
        y1=w4/4*c+y;
        _M2[y1]=ADD(MUL(_Gx[y1],_Gx[y1]),MUL(_Gy[y1],_Gy[y1]));
        if( c==0 ) continue; _m = CMPGT( _M2[y1], _M2[y] );
        _M2[y] = OR( AND(_m,_M2[y1]), ANDNOT(_m,_M2[y]) );
        _Gx[y] = OR( AND(_m,_Gx[y1]), ANDNOT(_m,_Gx[y]) );
        _Gy[y] = OR( AND(_m,_Gy[y1]), ANDNOT(_m,_Gy[y]) );
      }
    }
    // compute gradient mangitude (M) and normalize Gx // avoid the exception when arctan(Gy/Gx)
    for( y=0; y<w4/4; y++ ) {
      _m = MIN( RCPSQRT(_M2[y]), SET(1e10f) );
      _M2[y] = RCP(_m);
      if(O) _Gx[y] = MUL( MUL(_Gx[y],_m), SET(acMult) );
      if(O) _Gx[y] = XOR( _Gx[y], AND(_Gy[y], SET(-0.f)) );
    };
    memcpy( M+x*w, M2, w*sizeof(float) );
    // compute and store gradient orientation (O) via table lookup
    if( O!=0 ) for( y=0; y<w; y++ ) O[x*w+y] = acost[(int)Gx[y]];
    if( O!=0 && full ) {
      y1=((~size_t(O+x*w)+1)&15)/4; y=0;
      for( ; y<y1; y++ ) O[y+x*w]+=(Gy[y]<0)*(float)CV_PI;
      for( ; y<w-4; y+=4 ) STRu( O[y+x*w],
        ADD( LDu(O[y+x*w]), AND(CMPLT(LDu(Gy[y]),SET(0.f)),SET((float)CV_PI)) ) );
      for( ; y<w; y++ ) O[y+x*w]+=(Gy[y]<0)*(float)CV_PI;
    }
  }

  alFree(Gx); alFree(Gy); alFree(M2);
}




// normalize gradient magnitude at each location (uses sse)
void gradMagNorm( float *M,                     // output: M = M/(S + norm)
                  const float *S,               // input : Source Matrix
                  int h, int w, float norm )    // input : parameters
{
  __m128 *_M, *_S, _norm; int i=0, n=h*w, n4=n/4;
  _S = (__m128*) S; _M = (__m128*) M; _norm = SET(norm);
  bool sse = !(size_t(M)&15) && !(size_t(S)&15);
  if(sse) 
      for(; i<n4; i++) 
      { *_M=MUL(*_M,RCP(ADD(*_S++,_norm))); _M++; }
  if(sse) 
      i*=4; 
  for(; i<n; i++) M[i] /= (S[i] + norm);
}


// convolve one row of I by a 2rx1 triangle filter
void convTriY( float *I, float *O, int w, int r, int s ) 
{
  r++; float t, u; int j, r0=r-1, r1=r+1, r2=2*w-r, h0=r+1, h1=w-r+1, h2=w;
  u=t=I[0]; for( j=1; j<r; j++ ) u+=t+=I[j]; u=2*u-t; t=0;
  if( s==1 ) {
    O[0]=u; j=1;
    for(; j<h0; j++) O[j] = u += t += I[r-j]  + I[r0+j] - 2*I[j-1];
    for(; j<h1; j++) O[j] = u += t += I[j-r1] + I[r0+j] - 2*I[j-1];
    for(; j<h2; j++) O[j] = u += t += I[j-r1] + I[r2-j] - 2*I[j-1];
  } else {
    int k=(s-1)/2; h2=(w/s)*s; if(h0>h2) h0=h2; if(h1>h2) h1=h2;
    if(++k==s) { k=0; *O++=u; } j=1;
    for(;j<h0;j++) { u+=t+=I[r-j] +I[r0+j]-2*I[j-1]; if(++k==s){ k=0; *O++=u; }}
    for(;j<h1;j++) { u+=t+=I[j-r1]+I[r0+j]-2*I[j-1]; if(++k==s){ k=0; *O++=u; }}
    for(;j<h2;j++) { u+=t+=I[j-r1]+I[r2-j]-2*I[j-1]; if(++k==s){ k=0; *O++=u; }}
  }
}

void convTri_sse( const float *I, float *O, int width, int height, int r,int d = 1, int s=1 ) 
{
  r++; float nrm = 1.0f/(r*r*r*r); int i, j, k=(s-1)/2, h0, h1, w0;
  if(width%4==0) 
      h0=h1=width; 
  else 
  { h0=width-(width%4); h1=h0+4; } 
  w0=(height/s)*s;
  
  float *T=(float*) alMalloc(2*h1*sizeof(float),16), *U=T+h1;
  while( d-->0)
  {
     // initialize T and U
     for(j=0; j<h0; j+=4) STR(U[j], STR(T[j], LDu(I[j])));
     for(i=1; i<r; i++) for(j=0; j<h0; j+=4) INC(U[j],INC(T[j],LDu(I[j+i*width])));
     for(j=0; j<h0; j+=4) STR(U[j],MUL(nrm,(SUB(MUL(2,LD(U[j])),LD(T[j])))));
     for(j=0; j<h0; j+=4) STR(T[j],0);
     for(j=h0; j<width; j++ ) U[j]=T[j]=I[j];
     for(i=1; i<r; i++) for(j=h0; j<width; j++ ) U[j]+=T[j]+=I[j+i*width];
     for(j=h0; j<width; j++ ) { U[j] = nrm * (2*U[j]-T[j]); T[j]=0; }
     // prepare and convolve each column in turn
     k++; if(k==s) { k=0; convTriY(U,O,width,r-1,s); O+=width/s; }
     for( i=1; i<w0; i++ ) 
     {
       const float *Il=I+(i-1-r)*width; if(i<=r) Il=I+(r-i)*width; const float *Im=I+(i-1)*width;
       const float *Ir=I+(i-1+r)*width; if(i>height-r) Ir=I+(2*height-r-i)*width;
       for( j=0; j<h0; j+=4 ) {
         INC(T[j],ADD(LDu(Il[j]),LDu(Ir[j]),MUL(-2,LDu(Im[j]))));
         INC(U[j],MUL(nrm,LD(T[j])));
       }
       for( j=h0; j<width; j++ ) U[j]+=nrm*(T[j]+=Il[j]+Ir[j]-2*Im[j]);
       k++; if(k==s) { k=0; convTriY(U,O,width,r-1,s); O+=width/s; }
     }
     I+=width*height;
  }
  alFree(T);
}

// Constants for rgb2luv conversion and lookup table for y-> l conversion
template<class oT> oT* rgb2luv_setup( oT z, oT *mr, oT *mg, oT *mb,
  oT &minu, oT &minv, oT &un, oT &vn )
{
  // set constants for conversion
  const oT y0=(oT) ((6.0/29)*(6.0/29)*(6.0/29));
  const oT a= (oT) ((29.0/3)*(29.0/3)*(29.0/3));
  un=(oT) 0.197833; vn=(oT) 0.468331;
  mr[0]=(oT) 0.430574*z; mr[1]=(oT) 0.222015*z; mr[2]=(oT) 0.020183*z;
  mg[0]=(oT) 0.341550*z; mg[1]=(oT) 0.706655*z; mg[2]=(oT) 0.129553*z;
  mb[0]=(oT) 0.178325*z; mb[1]=(oT) 0.071330*z; mb[2]=(oT) 0.939180*z;
  oT maxi=(oT) 1.0/270; minu=-88*maxi; minv=-134*maxi;
  // build (padded) lookup table for y->l conversion assuming y in [0,1]
  static oT lTable[1064]; static bool lInit=false;
  if( lInit ) return lTable; oT y, l;
  for(int i=0; i<1025; i++) {
    y = (oT) (i/1024.0);
    l = y>y0 ? 116*(oT)pow((double)y,1.0/3.0)-16 : y*a;
    lTable[i] = l*maxi;
  }
  for(int i=1025; i<1064; i++) lTable[i]=lTable[i-1];
  lInit = true; return lTable;
}

// Convert from rgb to luv
template<class iT, class oT> void rgb2luv( const iT *I, 
											oT *J, 
											int n, 
											oT nrm ) 
{
  oT minu, minv, un, vn, mr[3], mg[3], mb[3];
  oT *lTable = rgb2luv_setup(nrm,mr,mg,mb,minu,minv,un,vn);
  oT *L=J, *U=L+n, *V=U+n; 
  const iT *R=I+2, *G=I+1, *B=I;			// opencv , B,G,R,B,G,R..
  for( int i=0; i<n; i++ ) 
  {
    oT r, g, b, x, y, z, l;
    r=(oT)*R; R=R+3; 
	g=(oT)*G; G=G+3;
	b=(oT)*B; B=B+3;
    x = mr[0]*r + mg[0]*g + mb[0]*b;
    y = mr[1]*r + mg[1]*g + mb[1]*b;
    z = mr[2]*r + mg[2]*g + mb[2]*b;
    l = lTable[(int)(y*1024)];
    *(L++) = l; z = 1/(x + 15*y + 3*z + (oT)1e-35);
    *(U++) = l * (13*4*x*z - 13*un) - minu;
    *(V++) = l * (13*9*y*z - 13*vn) - minv;
  }
}


// Convert from rgb to luv using sse
template<class iT> void rgb2luv_sse( iT *I, float *J, int n, float nrm ) 
{
  const int k=256; float R[k], G[k], B[k];
  if( (size_t(R)&15||size_t(G)&15||size_t(B)&15||size_t(I)&15||size_t(J)&15)
    || n%4>0 ) 
  { 
      rgb2luv(I,J,n,nrm); return; 
  }                      // data not align
  int i=0, i1, n1; float minu, minv, un, vn, mr[3], mg[3], mb[3];
  float *lTable = rgb2luv_setup(nrm,mr,mg,mb,minu,minv,un,vn);
  while( i<n ) 
  {
    n1 = i+k; if(n1>n) n1=n; float *J1=J+i; float *R1, *G1, *B1;
    /* ------------ RGB is now RRRRRGGGGGBBBBB ----------*/
    // convert to floats (and load input into cache)
    R1=R; G1=G; B1=B; 
    iT *Bi=I+i*3, *Gi=Bi+1, *Ri=Bi+2;
    for( i1=0; i1<(n1-i); i1++ ) 
    {
      R1[i1] = (float) (*Ri);Ri = Ri+3; 
      G1[i1] = (float) (*Gi);Gi = Gi+3; 
      B1[i1] = (float) (*Bi);Bi = Bi+3;
    }
    /* ------------ RGB is now RRRRRGGGGGBBBBB ----------*/
    // compute RGB -> XYZ
    for( int j=0; j<3; j++ ) 
    {
      __m128 _mr, _mg, _mb, *_J=(__m128*) (J1+j*n);
      __m128 *_R=(__m128*) R1, *_G=(__m128*) G1, *_B=(__m128*) B1;
      _mr=SET(mr[j]); _mg=SET(mg[j]); _mb=SET(mb[j]);
      for( i1=i; i1<n1; i1+=4 ) 
      {
        *(_J++) = ADD( ADD(MUL(*(_R++),_mr),MUL(*(_G++),_mg)),MUL(*(_B++),_mb));
      }
    }
    /* ---------------XXXXXXXYYYYYYYZZZZZZZZ now --------------- */

    { // compute XZY -> LUV (without doing L lookup/normalization)
      __m128 _c15, _c3, _cEps, _c52, _c117, _c1024, _cun, _cvn;
      _c15=SET(15.0f); _c3=SET(3.0f); _cEps=SET(1e-35f);
      _c52=SET(52.0f); _c117=SET(117.0f), _c1024=SET(1024.0f);
      _cun=SET(13*un); _cvn=SET(13*vn);
      __m128 *_X, *_Y, *_Z, _x, _y, _z;
      _X=(__m128*) J1; _Y=(__m128*) (J1+n); _Z=(__m128*) (J1+2*n);
      for( i1=i; i1<n1; i1+=4 ) 
      {
        _x = *_X; _y=*_Y; _z=*_Z;
        _z = RCP(ADD(_x,ADD(_cEps,ADD(MUL(_c15,_y),MUL(_c3,_z)))));
        *(_X++) = MUL(_c1024,_y);
        *(_Y++) = SUB(MUL(MUL(_c52,_x),_z),_cun);
        *(_Z++) = SUB(MUL(MUL(_c117,_y),_z),_cvn);
      }
    }
    { // perform lookup for L and finalize computation of U and V
      for( i1=i; i1<n1; i1++ ) J[i1] = lTable[(int)J[i1]];
      __m128 *_L, *_U, *_V, _l, _cminu, _cminv;
      _L=(__m128*) J1; _U=(__m128*) (J1+n); _V=(__m128*) (J1+2*n);
      _cminu=SET(minu); _cminv=SET(minv);
      for( i1=i; i1<n1; i1+=4 ) {
        _l = *(_L++);
        *_U = SUB(MUL(_l,*_U),_cminu); _U++;
        *_V = SUB(MUL(_l,*_V),_cminv); _V++;
      }
    }
    i = n1;
  }
}



bool feature_Pyramids::convt_2_luv( const Mat input_image, 
					  Mat &L_channel,
					  Mat &U_channel,
					  Mat &V_channel) const
{
	if( input_image.channels() != 3 || input_image.empty())
		return false;
	/* L U V channel is continunous in memory ~ */
	Mat luv_big = Mat::zeros( input_image.rows*3, input_image.cols, CV_32F);
	L_channel = luv_big.rowRange( 0, input_image.rows);
	U_channel = luv_big.rowRange( input_image.rows, input_image.rows*2);
	V_channel = luv_big.rowRange( input_image.rows*2, input_image.rows*3);
	int number_of_element = input_image.cols * input_image.rows;
	if( input_image.depth() == CV_8U)
		rgb2luv_sse( (const uchar*)(input_image.data), (float*)(luv_big.data), number_of_element, 1.0f/255);
	else if( input_image.depth() == CV_32F)
		rgb2luv_sse( (const float*)(input_image.data), (float*)(luv_big.data), number_of_element, 1.0f/255);
	else if( input_image.depth() == CV_64F)
		rgb2luv_sse( (const double*)(input_image.data), (float*)(luv_big.data), number_of_element, 1.0f/255);
	else
		return false;
	return true;
}

bool feature_Pyramids::computeGradMag(  const Mat &input_image,   
                                        const Mat &input_image2,
                                        const Mat &input_image3,
                                        Mat &mag,
                                        Mat &ori,
                                        bool full,
                                        int channel) const
{
    int dim = 1;
    if( input_image.depth() != CV_32F || input_image.empty() || channel < 0 || channel>2)
    {
        cout<<"image empty or depth!= CV_32F or channel < 0 || channel > 2"<<endl;
        return false;
    }
    if( !input_image2.empty() || !input_image3.empty() )
    {
        dim = 3;
        /* check the input data */
        float* data_start = (float*)input_image.data;
        float* data2_start = (float*)input_image2.data;
        float* data3_start = (float*)input_image3.data;
        if( data_start+input_image.cols*input_image.rows != data2_start || 
                data_start+2*input_image.cols*input_image.rows !=data3_start)
        {
            cout<<"for color image, the channels should be continuous in memory "<<endl;
        }
    }

    mag = Mat::zeros( input_image.size(), CV_32F);
    ori = Mat::zeros( input_image.size(), CV_32F);
    
    float* in_data = (float*)(input_image.data);
    if( channel!=0)
    {
        /* use specific channel */
        in_data += channel*input_image.cols*input_image.rows;
        dim = 1;
    }
    gradMag( (float*)(input_image.data), (float *)(mag.data), (float *)(ori.data), input_image.rows, 
                input_image.cols, dim, full );

    Mat smooth_mag;
    int norm_pad = 5;
    convTri( mag, smooth_mag, norm_pad, 1);
    float norm_const = 0.005;


    gradMagNorm( (float*)mag.data, (float*)smooth_mag.data, mag.rows, mag.cols, norm_const);

    return true;
}



Mat get_Km(int smooth )
{ 
    Mat dst(1, 2*smooth+1, CV_32FC1);
	for (int c=0;c<=smooth;c++)
	{
		dst.at<float>(0,c)=(float)((c+1)/((smooth+1.0)*(smooth+1.0)));
		dst.at<float>(0,2*smooth-c)=dst.at<float>(0,c);
	}
    return dst;
}
void feature_Pyramids::get_lambdas(vector<vector<Mat> > &chns_Pyramid,vector<double> &lambdas,vector<int> &real_scal,vector<double> &scales)const
{
	
	if (lam.empty()) 
	{
		Scalar lam_s;
		Scalar lam_ss;
		CV_Assert(chns_Pyramid.size()>=2);
		if (chns_Pyramid.size()>2)
		{
			//compute lambdas
			double size1,size2,lam_tmp;
			size1 =(double)chns_Pyramid[1][0].rows*chns_Pyramid[1][0].cols;
			size2 =(double)chns_Pyramid[2][0].rows*chns_Pyramid[2][0].cols;
			//compute luv	 
			for (int c=0;c<3;c++)
			{
				lam_s+=sum(chns_Pyramid[1][c]);		
				lam_ss+=sum(chns_Pyramid[2][c]);
			}
			lam_s=lam_s/(size1*3.0);
			lam_ss=lam_ss/(size2*3.0);
			lam_tmp=-cv::log(lam_ss.val[0]/lam_s.val[0])/cv::log(scales[real_scal[2]]/scales[real_scal[1]]);
			for (int c=0;c<3;c++)
			{
				lambdas.push_back(lam_tmp);
			}
			//compute  mag
			lam_s=sum(chns_Pyramid[1][4])/(size1*1.0);
			lam_ss=sum(chns_Pyramid[2][4])/(size2*1.0);
			lambdas.push_back(-cv::log(lam_ss.val[0]/lam_s.val[0])/cv::log(scales[real_scal[2]]/scales[real_scal[1]]));
			//compute grad_hist
			for (int c=4;c<10;c++)
			{
				lam_s+=sum(chns_Pyramid[1][c]);		
				lam_ss+=sum(chns_Pyramid[2][c]);
			}
			lam_s=lam_s/(size1*6.0);
			lam_ss=lam_ss/(size2*6.0);
			lam_tmp=-cv::log(lam_ss.val[0]/lam_s.val[0])/cv::log(scales[real_scal[2]]/scales[real_scal[1]]);
			for (int c=4;c<10;c++)
			{
				lambdas.push_back(lam_tmp);
			}
		}else{
			//compute lambdas
			double size0,size1,lam_tmp;
			size0 =(double)chns_Pyramid[0][0].rows*chns_Pyramid[0][0].cols;
			size1 =(double)chns_Pyramid[1][0].rows*chns_Pyramid[1][0].cols;
			//compute luv	 
			for (int c=0;c<3;c++)
			{
				lam_s+=sum(chns_Pyramid[0][c]);		
				lam_ss+=sum(chns_Pyramid[1][c]);
			}
			lam_s=lam_s/(size0*3.0);
			lam_ss=lam_ss/(size1*3.0);
			lam_tmp=-cv::log(lam_ss.val[0]/lam_s.val[0])/cv::log(scales[real_scal[1]]/scales[real_scal[0]]);
			for (int c=0;c<3;c++)
			{
				lambdas.push_back(lam_tmp);
			}
			//compute  mag
			lam_s=sum(chns_Pyramid[0][4])/(size0*1.0);
			lam_ss=sum(chns_Pyramid[1][4])/(size1*1.0);
			lambdas.push_back(-cv::log(lam_ss.val[0]/lam_s.val[0])/cv::log(scales[real_scal[1]]/scales[real_scal[0]]));
			//compute grad_hist
			for (int c=4;c<10;c++)
			{
				lam_s+=sum(chns_Pyramid[0][c]);		
				lam_ss+=sum(chns_Pyramid[1][c]);
			}
			lam_s=lam_s/(size0*6.0);
			lam_ss=lam_ss/(size1*6.0);
			lam_tmp=-cv::log(lam_ss.val[0]/lam_s.val[0])/cv::log(scales[real_scal[1]]/scales[real_scal[0]]);
			for (int c=4;c<10;c++)
			{
				lambdas.push_back(lam_tmp);
			}
		}
	}else{
		lambdas.resize(10);
		for(int n=0;n<3;n++)
		{
			lambdas[n]=lam[0];
		}
		lambdas[3]=lam[1];
		for(int n=4;n<10;n++)
		{
			lambdas[n]=lam[2];
		}	
	}
}

void feature_Pyramids::convTri( const Mat &src, Mat &dst,const Mat &Km) const   //1 opencv version
{
	CV_Assert(src.channels()<2);//不支持多通道
	filter2D(src,dst,src.depth(),Km,Point(-1,-1),0,IPL_BORDER_REFLECT);
	filter2D(dst,dst,src.depth(),Km.t(),Point(-1,-1),0,IPL_BORDER_REFLECT); 
	
}	

void feature_Pyramids::convTri( const Mat &src, Mat &dst, int conv_size, int dim) const      //2 sse version, faster
{
	CV_Assert(src.channels()==1 && conv_size > 0);//不支持多通道
    dst = Mat::zeros(  src.rows*dim, src.cols , CV_32F );
    if( conv_size > 1)
        convTri_sse( (const float*)( src.data), (float *)( dst.data), src.cols, src.rows, conv_size, dim);
    else
        convTri1( (const float*)( src.data), (float *)( dst.data), src.rows, src.cols, dim, (float)(conv_size) );
}

void feature_Pyramids::getscales(const Mat &img,vector<Size> &ap_size,vector<int> &real_scal,vector<double> &scales,vector<double> &scalesh,vector<double> &scalesw)const
{
	
	int nPerOct =m_opt.nPerOct;
	int nOctUp =m_opt.nOctUp;
	int shrink =m_opt.shrink;
	int nApprox=m_opt.nApprox;
	Size minDS =m_opt.minDS;

	int nscales=(int)floor(nPerOct*(nOctUp+log(min(img.cols/(minDS.width*1.0),img.rows/(minDS.height*1.0)))/log(2))+1);
	Size ap_tmp_size;

	CV_Assert(nApprox<nscales);

	double d0=(double)min(img.rows,img.cols);
	double d1=(double)max(img.rows,img.cols);
	for (double s=0;s<nscales;s++)
	{

		/*adjust ap_size*/
		double sc=pow(2.0,(-s)/nPerOct+nOctUp);
		double s0=(cvRound(d0*sc/shrink)*shrink-0.25*shrink)/d0;
		double s1=(cvRound(d0*sc/shrink)*shrink+0.25*shrink)/d0;
		double ss,es1,es2,a=10,val;
		for(int c=0;c<101;c++)
		{
			ss=(double)((s1-s0)*c/101+s0);
			es1=abs(d0*ss-cvRound(d0*ss/shrink)*shrink);
			es2=abs(d1*ss-cvRound(d1*ss/shrink)*shrink);
			if (max(es1,es2)<a)
			{
				a=max(es1,es2);
				val=ss;
			}
		}
		if (scales.empty())
		{
			/*all scales*/
			scales.push_back(val);
			scalesh.push_back(cvRound(img.rows*val/shrink)*shrink/(img.rows*1.0));
			scalesw.push_back(cvRound(img.cols*val/shrink)*shrink/(img.cols*1.0));
			/*save ap_size*/
			ap_size.push_back(Size(cvRound(((img.cols*val)/shrink)),cvRound(((img.rows*val)/shrink))));

		}else{
			if (val!=scales.back())
			{	
				/*all scales*/
				scales.push_back(val);
				scalesh.push_back(cvRound(img.rows*val/shrink)*shrink/(img.rows*1.0));
				scalesw.push_back(cvRound(img.cols*val/shrink)*shrink/(img.cols*1.0));
				/*save ap_size*/
				ap_size.push_back(Size(cvRound(((img.cols*val)/shrink)),cvRound(((img.rows*val)/shrink))));
			}
		}	
	}
   /*compute real & approx scales*/
	nscales=scales.size();

	for (int s=0;s<nscales;s++)
	{
		/*real scale*/
		if (((int)s%(nApprox+1)==0))
		{
			real_scal.push_back((int)s);
		}
		if ((s==(scales.size()-1)&&(s>real_scal[real_scal.size()-1])&&(s-real_scal[real_scal.size()-1]>(nApprox+1)/2)))
		{
			real_scal.push_back((int)s);
		}
	}
}
void feature_Pyramids::computeGradient(const Mat &img, 
                                        Mat& grad1,
                                        Mat& grad2,
                                        Mat& qangle1,
                                        Mat& qangle2,
                                        Mat& mag_sum_s) const
{
    TickMeter tk;
    tk.start();
	bool gammaCorrection = false;
    Size paddingTL=Size(0,0);
	Size paddingBR=Size(0,0);
	int nbins=m_opt.nbins;
	//CV_Assert( img.type() == CV_8U || img.type() == CV_8UC3 );
	CV_Assert( img.type() == CV_32F || img.type() == CV_32FC3 );

	Size gradsize(img.cols + paddingTL.width + paddingBR.width,
		img.rows + paddingTL.height + paddingBR.height);

	grad1.create(gradsize, CV_32FC1);  // <magnitude*(1-alpha)
	grad2.create(gradsize, CV_32FC1);  //  magnitude*alpha>
	qangle1.create(gradsize, CV_8UC1); // [0..nbins-1] - quantized gradient orientation
	qangle2.create(gradsize, CV_8UC1); // [0..nbins-1] - quantized gradient orientation

	Size wholeSize;
	Point roiofs;
	img.locateROI(wholeSize, roiofs);

	int x, y;
	int cn = img.channels();

	AutoBuffer<int> mapbuf(gradsize.width + gradsize.height + 4);
	int* xmap = (int*)mapbuf + 1;
	int* ymap = xmap + gradsize.width + 2;

	const int borderType = (int)BORDER_REFLECT_101;
	//! 1D interpolation function: returns coordinate of the "donor" pixel for the specified location p.
	for( x = -1; x < gradsize.width + 1; x++ )
		xmap[x] = borderInterpolate(x - paddingTL.width + roiofs.x,
		wholeSize.width, borderType) - roiofs.x;
	for( y = -1; y < gradsize.height + 1; y++ )
		ymap[y] = borderInterpolate(y - paddingTL.height + roiofs.y,
		wholeSize.height, borderType) - roiofs.y;

	// x- & y- derivatives for the whole row
	int width = gradsize.width;
	AutoBuffer<float> _dbuf(width*4);
	float* dbuf = _dbuf;
	Mat Dx(1, width, CV_32F, dbuf);
	Mat Dy(1, width, CV_32F, dbuf + width);
	Mat Mag(1, width, CV_32F, dbuf + width*2);
	Mat Angle(1, width, CV_32F, dbuf + width*3);

	int _nbins = nbins;
	float angleScale = (float)(_nbins/(CV_PI));//0~2*pi


	for( y = 0; y < gradsize.height; y++ )
	{
		const float* imgPtr  = (float*)(img.data + img.step*ymap[y]);
		const float* prevPtr = (float*)(img.data + img.step*ymap[y-1]);
		const float* nextPtr = (float*)(img.data + img.step*ymap[y+1]);

		float* gradPtr1 = (float*)grad1.ptr(y);
		float* gradPtr2 = (float*)grad2.ptr(y);
		uchar* qanglePtr1 = (uchar*)qangle1.ptr(y);
		uchar* qanglePtr2 = (uchar*)qangle2.ptr(y);

		if( cn == 1 )
		{
			for( x = 0; x < width; x++ )
			{
				int x1 = xmap[x];
				dbuf[x] = (float)(imgPtr[xmap[x+1]] - imgPtr[xmap[x-1]]);
				dbuf[width + x] = (float)(nextPtr[x1] - prevPtr[x1]); //??
			}
		}
		else
		{
			for( x = 0; x < width; x++ )
			{
				int x1 = xmap[x]*3;
				float dx0, dy0, dx, dy, mag0, mag;
				const float* p2 = imgPtr + xmap[x+1]*3;
				const float* p0 = imgPtr + xmap[x-1]*3;

				dx0 = (p2[2] - p0[2]);
				dy0 = (nextPtr[x1+2] - prevPtr[x1+2]);
				mag0 = dx0*dx0 + dy0*dy0;

				dx = (p2[1] - p0[1]);
				dy = (nextPtr[x1+1] - prevPtr[x1+1]);
				mag = dx*dx + dy*dy;

				if( mag0 < mag )
				{
					dx0 = dx;
					dy0 = dy;
					mag0 = mag;
				}

				dx = (p2[0] - p0[0]);
				dy = (nextPtr[x1] - prevPtr[x1]);
				mag = dx*dx + dy*dy;

				if( mag0 < mag )
				{
					dx0 = dx;
					dy0 = dy;
					mag0 = mag;
				}

				dbuf[x] = dx0;
				dbuf[x+width] = dy0;
			}
		}
		cartToPolar( Dx, Dy, Mag, Angle, false );

		for( x = 0; x < width; x++ )
		{
			float mag = dbuf[x+width*2];
			float act_ang = (dbuf[x+width*3] > CV_PI ? dbuf[x+width*3]-CV_PI: dbuf[x+width*3]);
			float angle = act_ang*angleScale;
			
			int hidx = cvFloor(angle);
			angle -= hidx;
			gradPtr1[x] = mag*(1.f - angle);
			gradPtr2[x] = mag*angle;

			if( hidx < 0 )
				hidx += _nbins;
			else if( hidx >= _nbins )
				hidx -= _nbins;
			assert( (unsigned)hidx < (unsigned)_nbins );

			qanglePtr1[x] = (uchar)hidx;
			hidx++;
			hidx &= hidx < _nbins ? -1 : 0;
			qanglePtr2[x] = (uchar)hidx;

		}
	}

    tk.stop();
	//cout<<"compute grad and ori time "<<tk.getTimeMilli()<<endl;

    Mat grad1_smooth, grad2_smooth;
    
    tk.reset();tk.start();
	double normConst=0.005; 

    /*  1 opencv version */
    //--------------------------------
	//convTri(grad1,grad1_smooth,m_normPad);
	//convTri(grad2,grad2_smooth,m_normPad);
    //--------------------------------

    /*  2 sse version */
    int norm_const = 5;
	convTri(grad1,grad1_smooth,norm_const, 1);
	convTri(grad2,grad2_smooth,norm_const, 1);

    tk.stop();
    //cout<<"smooth time "<<tk.getTimeMilli()<<endl;
    tk.reset();tk.start();
	//normalization
    
    /* 1 opencv  version */
    //--------------------------------
    //Mat norm_term = grad1_smooth + grad2_smooth + normConst;
	//grad1 = grad1/( norm_term );
	//grad2 = grad2/( norm_term );
    //--------------------------------

    /* 2 sse version, faster */
    //--------------------------------
    Mat norm_term = grad1_smooth + grad2_smooth;
    float *smooth_term = (float*)(norm_term.data);
    float *ptr_grad1 = (float*)grad1.data;
    float *ptr_grad2 = (float*)grad2.data;
    gradMagNorm( ptr_grad1, smooth_term, grad1.rows, grad1.cols, normConst);
    gradMagNorm( ptr_grad2, smooth_term, grad2.rows, grad2.cols, normConst);
    //--------------------------------

    tk.stop();
    //cout<<"add time "<<tk.getTimeMilli()<<endl;
	mag_sum_s=grad1+grad2;
}
void feature_Pyramids::computeChannels(const Mat &image,vector<Mat>& channels) const
{
	/*set para*/
	int nbins=m_opt.nbins;
	int binsize=m_opt.binsize;
	int shrink =m_opt.shrink;
	int smooth=m_opt.smooth;
	/* compute luv and push */
	Mat_<double> grad;
	Mat_<double> angles;
	Mat src,luv;
	
	//cv::TickMeter tm3;
	//tm3.start();
	int channels_addr_rows=(image.rows)/shrink;
	int channels_addr_cols=(image.cols)/shrink;
	Mat channels_addr=Mat::zeros((nbins+4)*channels_addr_rows,channels_addr_cols,CV_32FC1);
	if(image.channels() > 1)
	{
		src = Mat(image.rows, image.cols, CV_32FC3);
		image.convertTo(src, CV_32FC3, 1./255);
		cv::cvtColor(src, luv, CV_RGB2Luv);
	}else{
		src = Mat(image.rows, image.cols, CV_32FC1);
		image.convertTo(src, CV_32FC1, 1./255);
	}
	channels.clear();

	vector<Mat> luv_channels;
	luv_channels.resize(3);


	if(image.channels() > 1)
	{
		cv::split(luv, luv_channels);
		/*  0<L<100, -134<u<220 -140<v<122  */
		/*  normalize to [0, 1] */
		luv_channels[0] *= 1.0/354;
		convTri(luv_channels[0],luv_channels[0],m_km);
		luv_channels[1] = (luv_channels[1]+134)/(354.0);
		convTri(luv_channels[1],luv_channels[1],m_km);
	    luv_channels[2] = (luv_channels[2]+140)/(354.0);
		convTri(luv_channels[2],luv_channels[2],m_km);

		for( int i = 0; i < 3; ++i )
		{
			Mat channels_tmp=channels_addr.rowRange(i*channels_addr_rows,(i+1)*channels_addr_rows);
		    cv::resize(luv_channels[i],channels_tmp,channels_tmp.size(),0.0,0.0,1);
			channels.push_back(channels_tmp);
		}
	}
	/*compute gradient*/
	Mat mag_sum=channels_addr.rowRange(3*channels_addr_rows,4*channels_addr_rows);

	Mat luv_norm;
	cv::merge(luv_channels,luv_norm);
	
	Mat mag_sum_s;
	
    Mat mag1,mag2,ori1,ori2;
	computeGradient(luv_norm, mag1, mag2, ori1, ori2, mag_sum_s);//mzx 以上共花费64ms

	cv::resize(mag_sum_s,mag_sum,mag_sum.size(),0.0,0.0,INTER_AREA);
	channels.push_back(mag_sum);

	vector<Mat> bins_mat,bins_mat_tmp;
	int bins_mat_tmp_rows=mag1.rows/binsize;
	int bins_mat_tmp_cols=mag1.cols/binsize;
	for( int s=0;s<nbins;s++){
		Mat channels_tmp=channels_addr.rowRange((s+4)*channels_addr_rows,(s+5)*channels_addr_rows);
		if (binsize==shrink)
		{
			bins_mat_tmp.push_back(channels_tmp);
		}else{
			bins_mat.push_back(channels_tmp);
			bins_mat_tmp.push_back(Mat::zeros(bins_mat_tmp_rows,bins_mat_tmp_rows,CV_32FC1));
		}
	}
	//s*s---the number of the pixels of the spatial bin;
	float sc=binsize;
	/*split*/
#define GH \
	bins_mat_tmp[ori1.at<uchar>(row,col)].at<float>((row)/binsize,(col)/binsize)+=(mag1.at<float>(row,col)*(1.0/sc)*(1.0/sc));\
	bins_mat_tmp[ori2.at<uchar>(row,col)].at<float>((row)/binsize,(col)/binsize)+=(mag2.at<float>(row,col)*(1.0/sc)*(1.0/sc));
	for(int row=0;row<(mag1.rows/binsize*binsize);row++){
		for(int col=0;col<(mag1.cols/binsize*binsize);col++){GH;}}

	/*push*/
	for (int c=0;c < (int)nbins;c++)
	{
		/*resize*/
		if (binsize==shrink)
		{
			channels.push_back(bins_mat_tmp[c]);
		}else{
			cv::resize(bins_mat_tmp[c],bins_mat[c],bins_mat[c].size(),0.0,0.0,INTER_AREA);
			channels.push_back(bins_mat[c]);
		}
		
	}
 }
void feature_Pyramids:: chnsPyramid(const Mat &img,vector<vector<Mat> > &approxPyramid,vector<double> &scales,vector<double> &scalesh,vector<double> &scalesw) const
{

	int shrink =m_opt.shrink;
	int smooth =m_opt.smooth;
	int nApprox=m_opt.nApprox;
	Size pad =m_opt.pad;
	/*get scales*/
	Size ap_tmp_size;
	vector<Size> ap_size;
	vector<int> real_scal;
	//clear
	scales.clear();
	approxPyramid.clear();
	scalesh.clear();
	scalesw.clear();

	getscales(img,ap_size,real_scal,scales,scalesh,scalesw);
	Mat img_tmp;
	//Mat img_half;
	//compute real 
	vector<vector<Mat> > chns_Pyramid;
	int chns_num;
	for (int s_r=0;s_r<(int)real_scal.size();s_r++)
	{
		vector<Mat> chns;
		resize(img,img_tmp,ap_size[real_scal[s_r]]*shrink,0.0,0.0,INTER_AREA);
		/*if (img_half.empty())
		{
			resize(img,img_tmp,ap_size[real_scal[s_r]]*shrink,0.0,0.0,INTER_AREA);
		}else
		{
			resize(img_half,img_tmp,ap_size[real_scal[s_r]]*shrink,0.0,0.0,1);
		}
		if (abs(scales[real_scal[s_r]]-0.5)<=0.01)
		{
			img_half=img_tmp;
		}*/
		computeChannels(img_tmp,chns);
		chns_num=chns.size();
		chns_Pyramid.push_back(chns);
	}
	//compute lambdas
	vector<double> lambdas;
	if (nApprox!=0)
	{
		get_lambdas(chns_Pyramid,lambdas,real_scal,scales);
					
	}
	//compute based-scales
	vector<int> approx_scal;
	for (int s_r=0;s_r<scales.size();s_r++)
	{
			int tmp=s_r/(nApprox+1);
			if (s_r-real_scal[tmp]>((nApprox+1)/2))
			{
				approx_scal.push_back(real_scal[tmp+1]);
			}else{
				approx_scal.push_back(real_scal[tmp]);
			}	
	}
	//compute the filter
	//compute approxPyramid
	
	double ratio;
	for (int ap_id=0;ap_id<(int)approx_scal.size();ap_id++)
	{
		vector<Mat> approx_chns;
		approx_chns.clear();
		/*memory is consistent*/
		int approx_rows=ap_size[ap_id].height;
		int approx_cols=ap_size[ap_id].width;
		//pad
		int pad_T=pad.height/shrink;
		int pad_R=pad.width/shrink;
		Mat approx=Mat::zeros(10*(approx_rows+2*pad_T),approx_cols+2*pad_R,CV_32FC1);//因为chns_Pyramind是32F
		for(int n_chans=0;n_chans<chns_num;n_chans++)
		{
			Mat py_tmp=Mat::zeros(approx_rows,approx_cols,CV_32FC1);
	
			Mat py=approx.rowRange(n_chans*(approx_rows+2*pad_T),(n_chans+1)*(approx_rows+2*pad_T));//pad 以后的图像
	
			int ma=approx_scal[ap_id]/(nApprox+1);
			resize(chns_Pyramid[ma][n_chans],py_tmp,py_tmp.size(),0.0,0.0,INTER_AREA);
			
			if (nApprox!=0)
			{
				ratio=(double)pow(scales[ap_id]/scales[approx_scal[ap_id]],-lambdas[n_chans]);
				py_tmp=py_tmp*ratio;
			}
			//smooth channels, optionally pad and concatenate channels
			convTri(py_tmp,py_tmp,m_km);
			copyMakeBorder(py_tmp,py,pad_T,pad_T,pad_R,pad_R,IPL_BORDER_CONSTANT);
			approx_chns.push_back(py);
		}

	/*	float *add1 = (float*)approx_chns[0].data;
		for( int c=1;c<approx_chns.size();c++)
		{
		cout<<"pointer "<<(static_cast<void*>(approx_chns[c].data) == (static_cast<void*>(add1+c*approx_chns[0].cols*approx_chns[0].rows))? true :false)<<endl;
		}*/
		approxPyramid.push_back(approx_chns);
	}

    vector<int>().swap(real_scal);
	vector<int>().swap(approx_scal);
	vector<double>().swap(lambdas);
	vector<vector<Mat> >().swap(chns_Pyramid);
}
void feature_Pyramids:: chnsPyramid(const Mat &img,  vector<vector<Mat> > &chns_Pyramid,vector<double> &scales) const//nApprox==0时
{
	int shrink =m_opt.shrink;
	int smooth =m_opt.smooth;

	/*get scales*/
	Size ap_tmp_size;
	vector<Size> ap_size;
	vector<int> real_scal;
	vector<double> scalesh;
	vector<double> scalesw;
	//clear
	scales.clear();
	chns_Pyramid.clear();
	scalesh.clear();
	scalesw.clear();

	getscales(img,ap_size,real_scal,scales,scalesh,scalesw);

	Mat img_tmp;
	Mat img_half;
	//compute real 
	for (int s_r=0;s_r<(int)scales.size();s_r++)
	{
		vector<Mat> chns;
		cv::resize(img,img_tmp,ap_size[s_r]*shrink,0.0,0.0,INTER_AREA);
		/*if (img_half.empty())
		{
			resize(img,img_tmp,ap_size[real_scal[s_r]]*shrink,0.0,0.0,INTER_AREA);
		}else 
		{
			resize(img_half,img_tmp,ap_size[real_scal[s_r]]*shrink,0.0,0.0,1);
		}
		if (abs(scales[real_scal[s_r]]-0.5)<=0.01)
		{
			img_half=img_tmp;
		}*/
		computeChannels(img_tmp,chns);
		for (int c = 0; c < chns.size(); c++)
		{
			convTri(chns[c],chns[c],m_km);

		}
		chns_Pyramid.push_back(chns);
	}

	vector<Size>().swap(ap_size) ;
}
void feature_Pyramids::setParas(const detector_opt &in_para)
{
     m_opt=in_para;
}
void feature_Pyramids::compute_lambdas(const vector<Mat> &fold)
{
	/*get the Pyramid*/
	int nimages=fold.size();//the number of the images used to be train,must>=2;
	Mat image;
	CV_Assert(nimages>1);
	//配置参数
	feature_Pyramids feature_set;
	detector_opt in_opt;
	in_opt.nApprox=0;
	in_opt.pad=Size(0,0);
	feature_set.setParas(in_opt);

	vector<double> scal;
	vector<vector<Mat> > Pyramid;
	//the mean of the data
	vector<double> mean;	
	mean.resize(3);
	vector<vector<double> >Pyramid_mean;
	vector<vector<vector<double> > > Pyramid_set_mean;

	//test
	//nimages=2000;	
	for (int n=0;n<nimages;n++)
	{   
		image= fold[n];
		feature_set.chnsPyramid(image,Pyramid,scal);
		Pyramid_mean.clear();
		/*compute the mean of the n_type,where n_type=3(color,mag,gradhist)*/
		for (int n=0;n<Pyramid.size();n++)//比例scales
		{
			double size=Pyramid[n][0].rows*Pyramid[n][0].cols*1.0;
			Scalar lam_color,lam_mag,lam_hist;
			for (int p=0;p<3;p++)
			{
				lam_color+=sum(Pyramid[n][p]);
			}
			mean[0]=lam_color[0]/(size*3.0);
			lam_mag=sum(Pyramid[n][3]);
			mean[1]=lam_mag[0]/(size*1.0);

			for (int p = 0; p < 6; p++)
			{
				lam_hist+=sum(Pyramid[n][p+4]);
			}
			mean[2]=lam_hist[0]/(size*6.0);

			Pyramid_mean.push_back(mean);
		}
		Pyramid_set_mean.push_back(Pyramid_mean);

	}
	
	//scale.size() > 1
	CV_Assert(scal.size()>1);
	
	/*remove the small value when scale==1*/
	vector<vector<double> > base_data;
	base_data.resize(3);
	for (int i=0;i<3;i++)
	{
		for (int m=0;m<nimages;m++)
		{
			base_data[i].resize(nimages);
			base_data[i][m]=Pyramid_set_mean[m][0][i];
		}
	}

	double  maxdata0= *max_element( base_data[0].begin(),base_data[0].end());
	double  maxdata1= *max_element( base_data[1].begin(), base_data[1].end());
	double  maxdata2= *max_element( base_data[2].begin(), base_data[2].end());

	for (int n=0;n<nimages;n++)
	{
		if (base_data[0][n]<maxdata0/50.0 ) base_data[0][n]=0;
		if (base_data[1][n]<maxdata1/50.0 ) base_data[1][n]=0;
		if (base_data[2][n]<maxdata2/50.0 ) base_data[2][n]=0;						
	}
	/*get the lambdas*/
	//1.compute mus
	double num=0;//有效的图像数量
	Mat mus=Mat::zeros(scal.size()-1,3,CV_64FC1);//
	Mat s=Mat::ones(scal.size()-1,2,CV_64FC1);//0~35个log（scale）

	for (int r=0;r<scal.size()-1;r++)
	{
		s.at<double>(r,0)=log(scal[r+1])/log(2);
	}
	double sum;
	for (int m=0;m<scal.size()-1;m++)//m scales
	{
		for (int L=0;L<3;L++)//
		{
			num=0;
			sum=0;
			for (int n=0;n<nimages;n++)
			{
				if (base_data[L][n]!=0)//比较小的点就舍弃 该幅图像
				{	
					num++;
					sum+=(Pyramid_set_mean[n][m+1][L]/base_data[L][n]);//把nimages幅图像求平均
				}
			}
			mus.at<double>(m,L)=log(sum/num)/log(2);
		}
	}
	//compute lam;
	lam.clear();
	lam.resize(3);
	for (int n=0;n<3;n++)
	{
		Mat mus_omega=mus.colRange(n,n+1);
		Mat lam_omgea=(s.t()*s).inv()*(s.t())*mus_omega;
	    double a=-lam_omgea.at<double>(0,0);
		lam[n]=a;		
		cout<<"lam:"<<lam[n]<<endl;
	}
}
const detector_opt& feature_Pyramids::getParas() const
{
	 return m_opt;
}
feature_Pyramids::feature_Pyramids()
{
    int norm_pad_size = 5;
	m_normPad = get_Km(norm_pad_size);
	m_opt = detector_opt();
    m_km = get_Km( m_opt.smooth );
}
feature_Pyramids::~feature_Pyramids()
{
}

