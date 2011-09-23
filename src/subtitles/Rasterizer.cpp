/*
 *	Copyright (C) 2003-2006 Gabest
 *	http://www.gabest.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "stdafx.h"
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include "Rasterizer.h"
#include "SeparableFilter.h"
#include "..\SubPic\MemSubPic.h"
#include "xy_logger.h"
#include <boost/flyweight/key_value.hpp>

#ifndef _MAX	/* avoid collision with common (nonconforming) macros */
#define _MAX	(max)
#define _MIN	(min)
#define _IMPL_MAX max
#define _IMPL_MIN min
#else
#define _IMPL_MAX _MAX
#define _IMPL_MIN _MIN
#endif


//NOTE: signed or unsigned affects the result seriously
#define COMBINE_AYUV(a, y, u, v) ((((((((int)(a))<<8)|y)<<8)|u)<<8)|v)

#define SPLIT_AYUV(color, a, y, u, v) do { \
        *(v)=(color)&0xff; \
        *(u)=((color)>>8) &0xff; \
        *(y)=((color)>>16)&0xff;\
        *(a)=((color)>>24)&0xff;\
    } while(0)
                                    
class ass_synth_priv 
{
public:
    ass_synth_priv(const double sigma);
    ass_synth_priv(const ass_synth_priv& priv);
    ~ass_synth_priv();
    int generate_tables(double sigma);
    
    int g_r;
    int g_w;

    unsigned *g;
    unsigned *gt2;

    double sigma;
};

struct ass_synth_priv_key
{
    const double& operator()(const ass_synth_priv& x)const
    {
        return x.sigma;
    }
};

struct ass_tmp_buf
{
public:
    ass_tmp_buf(int size);
    ass_tmp_buf(const ass_tmp_buf& buf);
    ~ass_tmp_buf();
    int size;
    unsigned *tmp;
};

struct ass_tmp_buf_get_size
{
    const int& operator()(const ass_tmp_buf& buf)const
    {                                              
        return buf.size;
    }
};

static const unsigned int maxcolor = 255;
static const unsigned base = 256;

ass_synth_priv::ass_synth_priv(const double sigma)
{
    g_r = 0;
    g_w = 0;

    g = NULL;
    gt2 = NULL;

    this->sigma = 0;
    generate_tables(sigma);
}

ass_synth_priv::ass_synth_priv(const ass_synth_priv& priv):g_r(priv.g_r),g_w(priv.g_w)
{
    if (this->g_r) {
        this->g = (unsigned*)realloc(this->g, this->g_w * sizeof(unsigned));
        this->gt2 = (unsigned*)realloc(this->gt2, 256 * this->g_w * sizeof(unsigned));
        //if (this->g == null || this->gt2 == null) {
        //    return -1;
        //}
        memcpy(g, priv.g, this->g_w * sizeof(unsigned));
        memcpy(gt2, priv.gt2, 256 * this->g_w * sizeof(unsigned));
    }
}

ass_synth_priv::~ass_synth_priv()
{
    free(g); g=NULL;
    free(gt2); gt2=NULL;
}

int ass_synth_priv::generate_tables(double sigma)
{
    double a = -1 / (sigma * sigma * 2);
    int mx, i;
    double volume_diff, volume_factor = 0;
    unsigned volume;
    double * gaussian_kernel = NULL;

    if (this->sigma == sigma)
        return 0;
    else
        this->sigma = sigma;

    this->g_w = (int)ceil(sigma*3) | 1;
    this->g_r = this->g_w / 2;

    if (this->g_w) {
        this->g = (unsigned*)realloc(this->g, this->g_w * sizeof(unsigned));
        this->gt2 = (unsigned*)realloc(this->gt2, 256 * this->g_w * sizeof(unsigned));
        gaussian_kernel = (double*)malloc(this->g_w * sizeof(double));
        if (this->g == NULL || this->gt2 == NULL || gaussian_kernel == NULL) {          
            free(gaussian_kernel);
            return -1;
        }        
    }

    if (this->g_w) {
        for (i = 0; i < this->g_w; ++i) {
            gaussian_kernel[i] = exp(a * (i - this->g_r) * (i - this->g_r));
        }

        // gaussian curve with volume = 256
        for (volume_diff = 10000000; volume_diff > 0.0000001;
            volume_diff *= 0.5) {
                volume_factor += volume_diff;
                volume = 0;
                for (i = 0; i < this->g_w; ++i) {
                    this->g[i] = (unsigned) (gaussian_kernel[i] * volume_factor + .5);
                    volume += this->g[i];                    
                }
                if (volume > 0x10000)
                    volume_factor -= volume_diff;
        }
        volume = 0;
        for (i = 0; i < this->g_w; ++i) {
            this->g[i] = (unsigned) (gaussian_kernel[i] * volume_factor + .5);
            volume += this->g[i];
        }

        // gauss table:
        for (mx = 0; mx < this->g_w; mx++) {
            for (i = 0; i < 256; i++) {
                this->gt2[mx + i * this->g_w] = i * this->g[mx];
            }
        }
    }
    free(gaussian_kernel);
    return 0;
}

ass_tmp_buf::ass_tmp_buf(int size)
{
    tmp = (unsigned *)malloc(size * sizeof(unsigned));
    this->size = size;
}

ass_tmp_buf::ass_tmp_buf(const ass_tmp_buf& buf)
    :size(buf.size)
{
    tmp = (unsigned *)malloc(size * sizeof(unsigned));
}

ass_tmp_buf::~ass_tmp_buf()
{
    free(tmp);
}

/*
 * \brief gaussian blur.  an fast pure c implementation from libass.
 */
static void ass_gauss_blur(unsigned char *buffer, unsigned *tmp2,
                           int width, int height, int stride, const unsigned *m2,
                           int r, int mwidth)
{

    int x, y;

    unsigned char *s = buffer;
    unsigned *t = tmp2 + 1;
    for (y = 0; y < height; y++) {
        memset(t - 1, 0, (width + 1) * sizeof(*t));
        x = 0;
        if(x < r)//in case that r < 0
        {            
            const int src = s[x];
            if (src) {
                register unsigned *dstp = t + x - r;
                int mx;
                const unsigned *m3 = m2 + src * mwidth;
                unsigned sum = 0;
                for (mx = mwidth-1; mx >= r - x ; mx--) {                
                    sum += m3[mx];
                    dstp[mx] += sum;
                }
            }
        }

        for (x = 1; x < r; x++) {
            const int src = s[x];
            if (src) {
                register unsigned *dstp = t + x - r;
                int mx;
                const unsigned *m3 = m2 + src * mwidth;
                for (mx = r - x; mx < mwidth; mx++) {
                    dstp[mx] += m3[mx];
                }
            }
        }

        for (; x < width - r; x++) {
            const int src = s[x];
            if (src) {
                register unsigned *dstp = t + x - r;
                int mx;
                const unsigned *m3 = m2 + src * mwidth;
                for (mx = 0; mx < mwidth; mx++) {
                    dstp[mx] += m3[mx];
                }
            }
        }

        for (; x < width-1; x++) {
            const int src = s[x];
            if (src) {
                register unsigned *dstp = t + x - r;
                int mx;
                const int x2 = r + width - x;
                const unsigned *m3 = m2 + src * mwidth;
                for (mx = 0; mx < x2; mx++) {
                    dstp[mx] += m3[mx];
                }
            }
        }
        if(x==width-1) //important: x==width-1 failed, if r==0
        {
            const int src = s[x];
            if (src) {
                register unsigned *dstp = t + x - r;
                int mx;
                const int x2 = r + width - x;
                const unsigned *m3 = m2 + src * mwidth;
                unsigned sum = 0;
                for (mx = 0; mx < x2; mx++) {
                    sum += m3[mx];
                    dstp[mx] += sum;
                }
            }
        }

        s += stride;
        t += width + 1;
    }

    t = tmp2;
    for (x = 0; x < width; x++) {
        y = 0;
        if(y < r)//in case that r<0
        {            
            unsigned *srcp = t + y * (width + 1) + 1;
            int src = *srcp;
            if (src) {
                register unsigned *dstp = srcp - 1 + (mwidth -r +y)*(width + 1);
                const int src2 = (src + (1<<15)) >> 16;
                const unsigned *m3 = m2 + src2 * mwidth;
                unsigned sum = 0;
                int mx;
                *srcp = (1<<15);
                for (mx = mwidth-1; mx >=r - y ; mx--) {
                    sum += m3[mx];
                    *dstp += sum;
                    dstp -= width + 1;
                }
            }
        }
        for (y = 1; y < r; y++) {
            unsigned *srcp = t + y * (width + 1) + 1;
            int src = *srcp;
            if (src) {
                register unsigned *dstp = srcp - 1 + width + 1;
                const int src2 = (src + (1<<15)) >> 16;
                const unsigned *m3 = m2 + src2 * mwidth;

                int mx;
                *srcp = (1<<15);
                for (mx = r - y; mx < mwidth; mx++) {
                    *dstp += m3[mx];
                    dstp += width + 1;
                }
            }
        }
        for (; y < height - r; y++) {
            unsigned *srcp = t + y * (width + 1) + 1;
            int src = *srcp;
            if (src) {
                register unsigned *dstp = srcp - 1 - r * (width + 1);
                const int src2 = (src + (1<<15)) >> 16;
                const unsigned *m3 = m2 + src2 * mwidth;

                int mx;
                *srcp = (1<<15);
                for (mx = 0; mx < mwidth; mx++) {
                    *dstp += m3[mx];
                    dstp += width + 1;
                }
            }
        }
        for (; y < height-1; y++) {
            unsigned *srcp = t + y * (width + 1) + 1;
            int src = *srcp;
            if (src) {
                const int y2 = r + height - y;
                register unsigned *dstp = srcp - 1 - r * (width + 1);
                const int src2 = (src + (1<<15)) >> 16;
                const unsigned *m3 = m2 + src2 * mwidth;

                int mx;
                *srcp = (1<<15);
                for (mx = 0; mx < y2; mx++) {
                    *dstp += m3[mx];
                    dstp += width + 1;
                }
            }
        }
        if(y == height - 1)//important: y == height - 1 failed if r==0
        {
            unsigned *srcp = t + y * (width + 1) + 1;
            int src = *srcp;
            if (src) {
                const int y2 = r + height - y;
                register unsigned *dstp = srcp - 1 - r * (width + 1);
                const int src2 = (src + (1<<15)) >> 16;
                const unsigned *m3 = m2 + src2 * mwidth;
                unsigned sum = 0;
                int mx;
                *srcp = (1<<15);
                for (mx = 0; mx < y2; mx++) {
                    sum += m3[mx];
                    *dstp += sum;
                    dstp += width + 1;
                }
            }
        }
        t++;
    }

    t = tmp2;
    s = buffer;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            s[x] = t[x] >> 16;
        }
        s += stride;
        t += width + 1;
    }
}

/**
 * \brief blur with [[1,2,1]. [2,4,2], [1,2,1]] kernel.
 */
static void be_blur(unsigned char *buf, int w, int h, int stride)
{    
#pragma omp parallel for    
    for (int y = 0; y < h; y++) {
        unsigned char *temp_buf=buf+y*stride;
        int old_sum = 2 * temp_buf[0];
        int new_sum = 0;
        for (int x = 0; x < w - 1; x++) {
            new_sum = temp_buf[x] + temp_buf[x+1];
            temp_buf[x] = (old_sum + new_sum) >> 2;
            old_sum = new_sum;
        }
    }

#pragma omp parallel for
    for (int x = 0; x < w; x++) {
        unsigned char *temp_buf = buf + x;
        int old_sum = 2 * temp_buf[0];
        int new_sum = 0;
        for (int y = 0; y < h - 1; y++) {
            new_sum = temp_buf[0] + temp_buf[stride];
            temp_buf[0] = (old_sum + new_sum) >> 2;
            old_sum = new_sum;
            temp_buf += stride;
        }
    }
}

Rasterizer::Rasterizer() : mpPathTypes(NULL), mpPathPoints(NULL), mPathPoints(0)
{
    mPathOffsetX = mPathOffsetY = 0;
}

Rasterizer::~Rasterizer()
{
    _TrashPath();
}

void Rasterizer::_TrashPath()
{
    delete [] mpPathTypes;
    delete [] mpPathPoints;
    mpPathTypes = NULL;
    mpPathPoints = NULL;
    mPathPoints = 0;
}

void Rasterizer::_ReallocEdgeBuffer(int edges)
{
    mEdgeHeapSize = edges;
    mpEdgeBuffer = (Edge*)realloc(mpEdgeBuffer, sizeof(Edge)*edges);
}

void Rasterizer::_EvaluateBezier(int ptbase, bool fBSpline)
{
    const POINT* pt0 = mpPathPoints + ptbase;
    const POINT* pt1 = mpPathPoints + ptbase + 1;
    const POINT* pt2 = mpPathPoints + ptbase + 2;
    const POINT* pt3 = mpPathPoints + ptbase + 3;
    double x0 = pt0->x;
    double x1 = pt1->x;
    double x2 = pt2->x;
    double x3 = pt3->x;
    double y0 = pt0->y;
    double y1 = pt1->y;
    double y2 = pt2->y;
    double y3 = pt3->y;
    double cx3, cx2, cx1, cx0, cy3, cy2, cy1, cy0;
    if(fBSpline)
    {
        // 1   [-1 +3 -3 +1]
        // - * [+3 -6 +3  0]
        // 6   [-3  0 +3  0]
        //	   [+1 +4 +1  0]
        double _1div6 = 1.0/6.0;
        cx3 = _1div6*(-  x0+3*x1-3*x2+x3);
        cx2 = _1div6*( 3*x0-6*x1+3*x2);
        cx1 = _1div6*(-3*x0	   +3*x2);
        cx0 = _1div6*(   x0+4*x1+1*x2);
        cy3 = _1div6*(-  y0+3*y1-3*y2+y3);
        cy2 = _1div6*( 3*y0-6*y1+3*y2);
        cy1 = _1div6*(-3*y0     +3*y2);
        cy0 = _1div6*(   y0+4*y1+1*y2);
    }
    else // bezier
    {
        // [-1 +3 -3 +1]
        // [+3 -6 +3  0]
        // [-3 +3  0  0]
        // [+1  0  0  0]
        cx3 = -  x0+3*x1-3*x2+x3;
        cx2 =  3*x0-6*x1+3*x2;
        cx1 = -3*x0+3*x1;
        cx0 =    x0;
        cy3 = -  y0+3*y1-3*y2+y3;
        cy2 =  3*y0-6*y1+3*y2;
        cy1 = -3*y0+3*y1;
        cy0 =    y0;
    }
    //
    // This equation is from Graphics Gems I.
    //
    // The idea is that since we're approximating a cubic curve with lines,
    // any error we incur is due to the curvature of the line, which we can
    // estimate by calculating the maximum acceleration of the curve.  For
    // a cubic, the acceleration (second derivative) is a line, meaning that
    // the absolute maximum acceleration must occur at either the beginning
    // (|c2|) or the end (|c2+c3|).  Our bounds here are a little more
    // conservative than that, but that's okay.
    //
    // If the acceleration of the parametric formula is zero (c2 = c3 = 0),
    // that component of the curve is linear and does not incur any error.
    // If a=0 for both X and Y, the curve is a line segment and we can
    // use a step size of 1.
    double maxaccel1 = fabs(2*cy2) + fabs(6*cy3);
    double maxaccel2 = fabs(2*cx2) + fabs(6*cx3);
    double maxaccel = maxaccel1 > maxaccel2 ? maxaccel1 : maxaccel2;
    double h = 1.0;
    if(maxaccel > 8.0) h = sqrt(8.0 / maxaccel);
    if(!fFirstSet) {firstp.x = (LONG)cx0; firstp.y = (LONG)cy0; lastp = firstp; fFirstSet = true;}
    for(double t = 0; t < 1.0; t += h)
    {
        double x = cx0 + t*(cx1 + t*(cx2 + t*cx3));
        double y = cy0 + t*(cy1 + t*(cy2 + t*cy3));
        _EvaluateLine(lastp.x, lastp.y, (int)x, (int)y);
    }
    double x = cx0 + cx1 + cx2 + cx3;
    double y = cy0 + cy1 + cy2 + cy3;
    _EvaluateLine(lastp.x, lastp.y, (int)x, (int)y);
}

void Rasterizer::_EvaluateLine(int pt1idx, int pt2idx)
{
    const POINT* pt1 = mpPathPoints + pt1idx;
    const POINT* pt2 = mpPathPoints + pt2idx;
    _EvaluateLine(pt1->x, pt1->y, pt2->x, pt2->y);
}

void Rasterizer::_EvaluateLine(int x0, int y0, int x1, int y1)
{
    if(lastp.x != x0 || lastp.y != y0)
    {
        _EvaluateLine(lastp.x, lastp.y, x0, y0);
    }
    if(!fFirstSet) {firstp.x = x0; firstp.y = y0; fFirstSet = true;}
    lastp.x = x1;
    lastp.y = y1;
    if(y1 > y0)	// down
    {
        __int64 xacc = (__int64)x0 << 13;
        // prestep y0 down
        int dy = y1 - y0;
        int y = ((y0 + 3)&~7) + 4;
        int iy = y >> 3;
        y1 = (y1 - 5) >> 3;
        if(iy <= y1)
        {
            __int64 invslope = (__int64(x1 - x0) << 16) / dy;
            while(mEdgeNext + y1 + 1 - iy > mEdgeHeapSize)
                _ReallocEdgeBuffer(mEdgeHeapSize*2);
            xacc += (invslope * (y - y0)) >> 3;
            while(iy <= y1)
            {
                int ix = (int)((xacc + 32768) >> 16);
                mpEdgeBuffer[mEdgeNext].next = mpScanBuffer[iy];
                mpEdgeBuffer[mEdgeNext].posandflag = ix*2 + 1;
                mpScanBuffer[iy] = mEdgeNext++;
                ++iy;
                xacc += invslope;
            }
        }
    }
    else if(y1 < y0) // up
    {
        __int64 xacc = (__int64)x1 << 13;
        // prestep y1 down
        int dy = y0 - y1;
        int y = ((y1 + 3)&~7) + 4;
        int iy = y >> 3;
        y0 = (y0 - 5) >> 3;
        if(iy <= y0)
        {
            __int64 invslope = (__int64(x0 - x1) << 16) / dy;
            while(mEdgeNext + y0 + 1 - iy > mEdgeHeapSize)
                _ReallocEdgeBuffer(mEdgeHeapSize*2);
            xacc += (invslope * (y - y1)) >> 3;
            while(iy <= y0)
            {
                int ix = (int)((xacc + 32768) >> 16);
                mpEdgeBuffer[mEdgeNext].next = mpScanBuffer[iy];
                mpEdgeBuffer[mEdgeNext].posandflag = ix*2;
                mpScanBuffer[iy] = mEdgeNext++;
                ++iy;
                xacc += invslope;
            }
        }
    }
}

bool Rasterizer::BeginPath(HDC hdc)
{
    _TrashPath();
    return !!::BeginPath(hdc);
}

bool Rasterizer::EndPath(HDC hdc)
{
    ::CloseFigure(hdc);
    if(::EndPath(hdc))
    {
        mPathPoints = GetPath(hdc, NULL, NULL, 0);
        if(!mPathPoints)
            return true;
        mpPathTypes = (BYTE*)malloc(sizeof(BYTE) * mPathPoints);
        mpPathPoints = (POINT*)malloc(sizeof(POINT) * mPathPoints);
        if(mPathPoints == GetPath(hdc, mpPathPoints, mpPathTypes, mPathPoints))
            return true;
    }
    ::AbortPath(hdc);
    return false;
}

bool Rasterizer::PartialBeginPath(HDC hdc, bool bClearPath)
{
    if(bClearPath)
        _TrashPath();
    return !!::BeginPath(hdc);
}

bool Rasterizer::PartialEndPath(HDC hdc, long dx, long dy)
{
    ::CloseFigure(hdc);
    if(::EndPath(hdc))
    {
        int nPoints;
        BYTE* pNewTypes;
        POINT* pNewPoints;
        nPoints = GetPath(hdc, NULL, NULL, 0);
        if(!nPoints)
            return true;
        pNewTypes = (BYTE*)realloc(mpPathTypes, (mPathPoints + nPoints) * sizeof(BYTE));
        pNewPoints = (POINT*)realloc(mpPathPoints, (mPathPoints + nPoints) * sizeof(POINT));
        if(pNewTypes)
            mpPathTypes = pNewTypes;
        if(pNewPoints)
            mpPathPoints = pNewPoints;
        BYTE* pTypes = new BYTE[nPoints];
        POINT* pPoints = new POINT[nPoints];
        if(pNewTypes && pNewPoints && nPoints == GetPath(hdc, pPoints, pTypes, nPoints))
        {
            for(int i = 0; i < nPoints; ++i)
            {
                mpPathPoints[mPathPoints + i].x = pPoints[i].x + dx;
                mpPathPoints[mPathPoints + i].y = pPoints[i].y + dy;
                mpPathTypes[mPathPoints + i] = pTypes[i];
            }
            mPathPoints += nPoints;
            delete[] pTypes;
            delete[] pPoints;
            return true;
        }
        else
            DebugBreak();
        delete[] pTypes;
        delete[] pPoints;
    }
    ::AbortPath(hdc);
    return false;
}

bool Rasterizer::ScanConvert()
{
    int lastmoveto = -1;
    int i;
    // Drop any outlines we may have.
    mOutline.clear();
    mWideOutline.clear();
    mWideBorder = 0;
    // Determine bounding box
    if(!mPathPoints)
    {
        mPathOffsetX = mPathOffsetY = 0;
        mWidth = mHeight = 0;
        return 0;
    }
    int minx = INT_MAX;
    int miny = INT_MAX;
    int maxx = INT_MIN;
    int maxy = INT_MIN;
    for(i=0; i<mPathPoints; ++i)
    {
        int ix = mpPathPoints[i].x;
        int iy = mpPathPoints[i].y;
        if(ix < minx) minx = ix;
        if(ix > maxx) maxx = ix;
        if(iy < miny) miny = iy;
        if(iy > maxy) maxy = iy;
    }
    minx = (minx >> 3) & ~7;
    miny = (miny >> 3) & ~7;
    maxx = (maxx + 7) >> 3;
    maxy = (maxy + 7) >> 3;
    for(i=0; i<mPathPoints; ++i)
    {
        mpPathPoints[i].x -= minx*8;
        mpPathPoints[i].y -= miny*8;
    }
    if(minx > maxx || miny > maxy)
    {
        mWidth = mHeight = 0;
        mPathOffsetX = mPathOffsetY = 0;
        _TrashPath();
        return true;
    }
    mWidth = maxx + 1 - minx;
    mHeight = maxy + 1 - miny;
    mPathOffsetX = minx;
    mPathOffsetY = miny;
    // Initialize edge buffer.  We use edge 0 as a sentinel.
    mEdgeNext = 1;
    mEdgeHeapSize = 2048;
    mpEdgeBuffer = (Edge*)malloc(sizeof(Edge)*mEdgeHeapSize);
    // Initialize scanline list.
    mpScanBuffer = new unsigned int[mHeight];
    memset(mpScanBuffer, 0, mHeight*sizeof(unsigned int));
    // Scan convert the outline.  Yuck, Bezier curves....
    // Unfortunately, Windows 95/98 GDI has a bad habit of giving us text
    // paths with all but the first figure left open, so we can't rely
    // on the PT_CLOSEFIGURE flag being used appropriately.
    fFirstSet = false;
    firstp.x = firstp.y = 0;
    lastp.x = lastp.y = 0;
    for(i=0; i<mPathPoints; ++i)
    {
        BYTE t = mpPathTypes[i] & ~PT_CLOSEFIGURE;
        switch(t)
        {
        case PT_MOVETO:
            if(lastmoveto >= 0 && firstp != lastp)
                _EvaluateLine(lastp.x, lastp.y, firstp.x, firstp.y);
            lastmoveto = i;
            fFirstSet = false;
            lastp = mpPathPoints[i];
            break;
        case PT_MOVETONC:
            break;
        case PT_LINETO:
            if(mPathPoints - (i-1) >= 2) _EvaluateLine(i-1, i);
            break;
        case PT_BEZIERTO:
            if(mPathPoints - (i-1) >= 4) _EvaluateBezier(i-1, false);
            i += 2;
            break;
        case PT_BSPLINETO:
            if(mPathPoints - (i-1) >= 4) _EvaluateBezier(i-1, true);
            i += 2;
            break;
        case PT_BSPLINEPATCHTO:
            if(mPathPoints - (i-3) >= 4) _EvaluateBezier(i-3, true);
            break;
        }
    }
    if(lastmoveto >= 0 && firstp != lastp)
        _EvaluateLine(lastp.x, lastp.y, firstp.x, firstp.y);
    // Free the path since we don't need it anymore.
    _TrashPath();
    // Convert the edges to spans.  We couldn't do this before because some of
    // the regions may have winding numbers >+1 and it would have been a pain
    // to try to adjust the spans on the fly.  We use one heap to detangle
    // a scanline's worth of edges from the singly-linked lists, and another
    // to collect the actual scans.
    std::vector<int> heap;
    mOutline.reserve(mEdgeNext / 2);
    __int64 y = 0;
    for(y=0; y<mHeight; ++y)
    {
        int count = 0;
        // Detangle scanline into edge heap.
        for(unsigned ptr = (unsigned)(mpScanBuffer[y]&0xffffffff); ptr; ptr = mpEdgeBuffer[ptr].next)
        {
            heap.push_back(mpEdgeBuffer[ptr].posandflag);
        }
        // Sort edge heap.  Note that we conveniently made the opening edges
        // one more than closing edges at the same spot, so we won't have any
        // problems with abutting spans.
        std::sort(heap.begin(), heap.end()/*begin() + heap.size()*/);
        // Process edges and add spans.  Since we only check for a non-zero
        // winding number, it doesn't matter which way the outlines go!
        std::vector<int>::iterator itX1 = heap.begin();
        std::vector<int>::iterator itX2 = heap.end(); // begin() + heap.size();
        int x1, x2;
        for(; itX1 != itX2; ++itX1)
        {
            int x = *itX1;
            if(!count)
                x1 = (x>>1);
            if(x&1)
                ++count;
            else
                --count;
            if(!count)
            {
                x2 = (x>>1);
                if(x2>x1)
                    mOutline.push_back(std::pair<__int64,__int64>((y<<32)+x1+0x4000000040000000i64, (y<<32)+x2+0x4000000040000000i64)); // G: damn Avery, this is evil! :)
            }
        }
        heap.clear();
    }
    // Dump the edge and scan buffers, since we no longer need them.
    free(mpEdgeBuffer);
    delete [] mpScanBuffer;
    // All done!
    return true;
}

using namespace std;

void Rasterizer::_OverlapRegion(tSpanBuffer& dst, tSpanBuffer& src, int dx, int dy)
{
    tSpanBuffer temp;
    temp.reserve(dst.size() + src.size());
    dst.swap(temp);
    tSpanBuffer::iterator itA = temp.begin();
    tSpanBuffer::iterator itAE = temp.end();
    tSpanBuffer::iterator itB = src.begin();
    tSpanBuffer::iterator itBE = src.end();
    // Don't worry -- even if dy<0 this will still work! // G: hehe, the evil twin :)
    unsigned __int64 offset1 = (((__int64)dy)<<32) - dx;
    unsigned __int64 offset2 = (((__int64)dy)<<32) + dx;
    while(itA != itAE && itB != itBE)
    {
        if((*itB).first + offset1 < (*itA).first)
        {
            // B span is earlier.  Use it.
            unsigned __int64 x1 = (*itB).first + offset1;
            unsigned __int64 x2 = (*itB).second + offset2;
            ++itB;
            // B spans don't overlap, so begin merge loop with A first.
            for(;;)
            {
                // If we run out of A spans or the A span doesn't overlap,
                // then the next B span can't either (because B spans don't
                // overlap) and we exit.
                if(itA == itAE || (*itA).first > x2)
                    break;
                do {x2 = _MAX(x2, (*itA++).second);}
                while(itA != itAE && (*itA).first <= x2);
                // If we run out of B spans or the B span doesn't overlap,
                // then the next A span can't either (because A spans don't
                // overlap) and we exit.
                if(itB == itBE || (*itB).first + offset1 > x2)
                    break;
                do {x2 = _MAX(x2, (*itB++).second + offset2);}
                while(itB != itBE && (*itB).first + offset1 <= x2);
            }
            // Flush span.
            dst.push_back(tSpan(x1, x2));
        }
        else
        {
            // A span is earlier.  Use it.
            unsigned __int64 x1 = (*itA).first;
            unsigned __int64 x2 = (*itA).second;
            ++itA;
            // A spans don't overlap, so begin merge loop with B first.
            for(;;)
            {
                // If we run out of B spans or the B span doesn't overlap,
                // then the next A span can't either (because A spans don't
                // overlap) and we exit.
                if(itB == itBE || (*itB).first + offset1 > x2)
                    break;
                do {x2 = _MAX(x2, (*itB++).second + offset2);}
                while(itB != itBE && (*itB).first + offset1 <= x2);
                // If we run out of A spans or the A span doesn't overlap,
                // then the next B span can't either (because B spans don't
                // overlap) and we exit.
                if(itA == itAE || (*itA).first > x2)
                    break;
                do {x2 = _MAX(x2, (*itA++).second);}
                while(itA != itAE && (*itA).first <= x2);
            }
            // Flush span.
            dst.push_back(tSpan(x1, x2));
        }
    }
    // Copy over leftover spans.
    while(itA != itAE)
        dst.push_back(*itA++);
    while(itB != itBE)
    {
        dst.push_back(tSpan((*itB).first + offset1, (*itB).second + offset2));
        ++itB;
    }
}

bool Rasterizer::CreateWidenedRegion(int rx, int ry)
{
    if(rx < 0) rx = 0;
    if(ry < 0) ry = 0;
    mWideBorder = max(rx,ry);
    if (ry > 0)
    {
        // Do a half circle.
        // _OverlapRegion mirrors this so both halves are done.
        for(int y = -ry; y <= ry; ++y)
        {
            int x = (int)(0.5 + sqrt(float(ry*ry - y*y)) * float(rx)/float(ry));
            _OverlapRegion(mWideOutline, mOutline, x, y);
        }
    }
    else if (ry == 0 && rx > 0)
    {
        // There are artifacts if we don't make at least two overlaps of the line, even at same Y coord
        _OverlapRegion(mWideOutline, mOutline, rx, 0);
        _OverlapRegion(mWideOutline, mOutline, rx, 0);
    }
    return true;
}

void Rasterizer::DeleteOutlines()
{
    mWideOutline.clear();
    mOutline.clear();
}

bool Rasterizer::Rasterize(int xsub, int ysub, int fBlur, double fGaussianBlur, Overlay* overlay)
{
    using namespace ::boost::flyweights;
        
    if(!overlay)
    {
        return false;
    }
    overlay->CleanUp();

    if(!mWidth || !mHeight)
    {
        return true;
    }
    xsub &= 7;
    ysub &= 7;
    //xsub = ysub = 0;
    int width = mWidth + xsub;
    int height = mHeight + ysub;
    overlay->mOffsetX = mPathOffsetX - xsub;
    overlay->mOffsetY = mPathOffsetY - ysub;
    mWideBorder = (mWideBorder+7)&~7;
    if(!mWideOutline.empty() || fBlur || fGaussianBlur > 0)
    {
        int bluradjust = 0;
        if (fGaussianBlur > 0)
            bluradjust += (int)(fGaussianBlur*3*8 + 0.5) | 1;
        if (fBlur)
            bluradjust += 8;
        // Expand the buffer a bit when we're blurring, since that can also widen the borders a bit
        bluradjust = (bluradjust+7)&~7;
        width += 2*mWideBorder + bluradjust*2;
        height += 2*mWideBorder + bluradjust*2;
        xsub += mWideBorder + bluradjust;
        ysub += mWideBorder + bluradjust;
        overlay->mOffsetX -= mWideBorder + bluradjust;
        overlay->mOffsetY -= mWideBorder + bluradjust;
    }
    overlay->mOverlayWidth = ((width+7)>>3) + 1;
    overlay->mOverlayHeight = ((height+7)>>3) + 1;
    overlay->mOverlayPitch = (overlay->mOverlayWidth+15)&~15;
        
    overlay->mpOverlayBuffer.base = (byte*)xy_malloc(2 * overlay->mOverlayPitch * overlay->mOverlayHeight);
    memset(overlay->mpOverlayBuffer.base, 0, 2 * overlay->mOverlayPitch * overlay->mOverlayHeight);
    overlay->mpOverlayBuffer.body = overlay->mpOverlayBuffer.base;
    overlay->mpOverlayBuffer.border = overlay->mpOverlayBuffer.base + overlay->mOverlayPitch * overlay->mOverlayHeight;        

    // Are we doing a border?
    tSpanBuffer* pOutline[2] = {&mOutline, &mWideOutline};
    for(int i = countof(pOutline)-1; i >= 0; i--)
    {
        tSpanBuffer::iterator it = pOutline[i]->begin();
        tSpanBuffer::iterator itEnd = pOutline[i]->end();
        byte* plan_selected = i==0 ? overlay->mpOverlayBuffer.body : overlay->mpOverlayBuffer.border;
        int pitch = overlay->mOverlayPitch;
        for(; it!=itEnd; ++it)
        {
            int y = (int)(((*it).first >> 32) - 0x40000000 + ysub);
            int x1 = (int)(((*it).first & 0xffffffff) - 0x40000000 + xsub);
            int x2 = (int)(((*it).second & 0xffffffff) - 0x40000000 + xsub);
            if(x2 > x1)
            {
                int first = x1>>3;
                int last = (x2-1)>>3;
                byte* dst = plan_selected + (pitch*(y>>3) + first);
                if(first == last)
                    *dst += x2-x1;
                else
                {
                    *dst += ((first+1)<<3) - x1;
                    dst += 1;
                    while(++first < last)
                    {
                        *dst += 0x08;
                        dst += 1;
                    }
                    *dst += x2 - (last<<3);
                }
            }
        }
    }
    // Do some gaussian blur magic    
    if (fGaussianBlur > 0.1)//(fGaussianBlur > 0) return true even if fGaussianBlur very small
    {
        byte* plan_selected= mWideOutline.empty() ? overlay->mpOverlayBuffer.body : overlay->mpOverlayBuffer.border;
        flyweight<key_value<double, ass_synth_priv, ass_synth_priv_key>, no_locking> fw_priv_blur(fGaussianBlur);
        const ass_synth_priv& priv_blur = fw_priv_blur.get();
        if (overlay->mOverlayWidth>=priv_blur.g_w && overlay->mOverlayHeight>=priv_blur.g_w)
        {                             
            flyweight<key_value<int, ass_tmp_buf, ass_tmp_buf_get_size>, no_locking> tmp_buf((overlay->mOverlayWidth+1)*(overlay->mOverlayHeight+1));
            ass_gauss_blur(plan_selected, tmp_buf.get().tmp, overlay->mOverlayWidth, overlay->mOverlayHeight, overlay->mOverlayPitch, 
                priv_blur.gt2, priv_blur.g_r, priv_blur.g_w);
        }
    }

    for (int pass = 0; pass < fBlur; pass++)
    {
        if(overlay->mOverlayWidth >= 3 && overlay->mOverlayHeight >= 3)
        {
            int pitch = overlay->mOverlayPitch;
            byte* plan_selected= mWideOutline.empty() ? overlay->mpOverlayBuffer.body : overlay->mpOverlayBuffer.border;
            be_blur(plan_selected+1+pitch, overlay->mOverlayWidth-2, overlay->mOverlayHeight-2, pitch);
        }
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////

static __forceinline void pixmix(DWORD *dst, DWORD color, DWORD alpha)
{
    int a = (((alpha)*(color>>24))>>6)&0xff;
    // Make sure both a and ia are in range 1..256 for the >>8 operations below to be correct
    int ia = 256-a;
    a+=1;
    *dst = ((((*dst&0x00ff00ff)*ia + (color&0x00ff00ff)*a)&0xff00ff00)>>8)
           | ((((*dst&0x0000ff00)*ia + (color&0x0000ff00)*a)&0x00ff0000)>>8)
           | ((((*dst>>8)&0x00ff0000)*ia)&0xff000000);
}

static __forceinline void pixmix2(DWORD *dst, DWORD color, DWORD shapealpha, DWORD clipalpha)
{
    int a = (((shapealpha)*(clipalpha)*(color>>24))>>12)&0xff;
    int ia = 256-a;
    a+=1;
    *dst = ((((*dst&0x00ff00ff)*ia + (color&0x00ff00ff)*a)&0xff00ff00)>>8)
           | ((((*dst&0x0000ff00)*ia + (color&0x0000ff00)*a)&0x00ff0000)>>8)
           | ((((*dst>>8)&0x00ff0000)*ia)&0xff000000);
}

#include <xmmintrin.h>
#include <emmintrin.h>

static __forceinline void pixmix_sse2(DWORD* dst, DWORD color, DWORD alpha)
{
//    alpha = (((alpha) * (color>>24)) >> 6) & 0xff;
    color &= 0xffffff;
    __m128i zero = _mm_setzero_si128();
    __m128i a = _mm_set1_epi32(((alpha+1) << 16) | (0x100 - alpha));
    __m128i d = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*dst), zero);
    __m128i s = _mm_unpacklo_epi8(_mm_cvtsi32_si128(color), zero);
    __m128i r = _mm_unpacklo_epi16(d, s);
    r = _mm_madd_epi16(r, a);
    r = _mm_srli_epi32(r, 8);
    r = _mm_packs_epi32(r, r);
    r = _mm_packus_epi16(r, r);
    *dst = (DWORD)_mm_cvtsi128_si32(r);
}

static __forceinline void pixmix2_sse2(DWORD* dst, DWORD color, DWORD shapealpha, DWORD clipalpha)
{
    int alpha = (((shapealpha)*(clipalpha)*(color>>24))>>12)&0xff;
    color &= 0xffffff;
    __m128i zero = _mm_setzero_si128();
    __m128i a = _mm_set1_epi32(((alpha+1) << 16) | (0x100 - alpha));
    __m128i d = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*dst), zero);
    __m128i s = _mm_unpacklo_epi8(_mm_cvtsi32_si128(color), zero);
    __m128i r = _mm_unpacklo_epi16(d, s);
    r = _mm_madd_epi16(r, a);
    r = _mm_srli_epi32(r, 8);
    r = _mm_packs_epi32(r, r);
    r = _mm_packus_epi16(r, r);
    *dst = (DWORD)_mm_cvtsi128_si32(r);
}

#include <mmintrin.h>

// Calculate a - b clamping to 0 instead of underflowing
static __forceinline DWORD safe_subtract(DWORD a, DWORD b)
{
    __m64 ap = _mm_cvtsi32_si64(a);
    __m64 bp = _mm_cvtsi32_si64(b);
    __m64 rp = _mm_subs_pu16(ap, bp);
    DWORD r = (DWORD)_mm_cvtsi64_si32(rp);
    _mm_empty();
    return r;
    //return (b > a) ? 0 : a - b;
}

// For CPUID usage in Rasterizer::Draw
#include "../dsutil/vd.h"

static const __int64 _00ff00ff00ff00ff = 0x00ff00ff00ff00ffi64;

// Render a subpicture onto a surface.
// spd is the surface to render on.
// clipRect is a rectangular clip region to render inside.
// pAlphaMask is an alpha clipping mask.
// xsub and ysub ???
// switchpts seems to be an array of fill colours interlaced with coordinates.
//    switchpts[i*2] contains a colour and switchpts[i*2+1] contains the coordinate to use that colour from
// fBody tells whether to render the body of the subs.
// fBorder tells whether to render the border of the subs.
CRect Rasterizer::Draw(SubPicDesc& spd, Overlay* overlay, CRect& clipRect, byte* pAlphaMask, 
    int xsub, int ysub, const DWORD* switchpts, bool fBody, bool fBorder)
{
    CRect bbox(0, 0, 0, 0);
    if(!switchpts || !fBody && !fBorder) return(bbox);
    struct DM
    {
        enum
        {
            SSE2 = 1,
            ALPHA_MASK = 1<<1,
            SINGLE_COLOR = 1<<2,
            BODY = 1<<3,
            YV12 = 1<<4
        };
    };
    // CPUID from VDub
    bool fSSE2 = !!(g_cpuid.m_flags & CCpuID::sse2);
    bool fSingleColor = (switchpts[1] == 0xffffffff);
    bool fYV12 = spd.type==MSP_YV12 || spd.type==MSP_IYUV;
    int draw_method = 0;
//	if(pAlphaMask)
//		draw_method |= DM::ALPHA_MASK;
    if(fSingleColor)
        draw_method |= DM::SINGLE_COLOR;
//	if(fBody)
//		draw_method |= DM::BODY;
    if(fSSE2)
        draw_method |= DM::SSE2;
    if(fYV12)
        draw_method |= DM::YV12;
    // clip
    // Limit drawn area to intersection of rendering surface and rectangular clip area
    CRect r(0, 0, spd.w, spd.h);
    r &= clipRect;
    // Remember that all subtitle coordinates are specified in 1/8 pixels
    // (x+4)>>3 rounds to nearest whole pixel.
    // ??? What is xsub, ysub, mOffsetX and mOffsetY ?
    int overlayPitch = overlay->mOverlayPitch;
    int x = (xsub + overlay->mOffsetX + 4)>>3;
    int y = (ysub + overlay->mOffsetY + 4)>>3;
    int w = overlay->mOverlayWidth;
    int h = overlay->mOverlayHeight;
    int xo = 0, yo = 0;
    // Again, limiting?
    if(x < r.left) {xo = r.left-x; w -= r.left-x; x = r.left;}
    if(y < r.top) {yo = r.top-y; h -= r.top-y; y = r.top;}
    if(x+w > r.right) w = r.right-x;
    if(y+h > r.bottom) h = r.bottom-y;
    // Check if there's actually anything to render
    if(w <= 0 || h <= 0) return(bbox);
    bbox.SetRect(x, y, x+w, y+h);
    bbox &= CRect(0, 0, spd.w, spd.h);
    // draw
    // Grab the first colour
    DWORD color = switchpts[0];
    byte* s_base = (byte*)xy_malloc(overlay->mOverlayPitch * overlay->mOverlayHeight);
    overlay->FillAlphaMash(s_base, fBody, fBorder, xo, yo, w, h, pAlphaMask==NULL ? NULL : pAlphaMask + spd.w * y + x, spd.w,
        fSingleColor?(color>>24):0xff );
    const byte* s = s_base + overlay->mOverlayPitch*yo + xo;

    // How would this differ from src?
    unsigned long* dst = (unsigned long *)(((char *)spd.bits + spd.pitch * y) + ((x*spd.bpp)>>3));

    // Every remaining line in the bitmap to be rendered...
    switch(draw_method)
    {
    case   DM::SINGLE_COLOR |   DM::SSE2 | 0*DM::YV12 :
    {
        while(h--)
        {
            for(int wt=0; wt<w; ++wt)
                // The <<6 is due to pixmix expecting the alpha parameter to be
                // the multiplication of two 6-bit unsigned numbers but we
                // only have one here. (No alpha mask.)
                pixmix_sse2(&dst[wt], color, s[wt]);
            s += overlayPitch;
            dst = (unsigned long *)((char *)dst + spd.pitch);
        }
    }
    break;
    case   DM::SINGLE_COLOR | 0*DM::SSE2 | 0*DM::YV12 :
    {
        while(h--)
        {
            for(int wt=0; wt<w; ++wt)
                pixmix(&dst[wt], color, s[wt]);
            s += overlayPitch;
            dst = (unsigned long *)((char *)dst + spd.pitch);
        }
    }
    break;
    case 0*DM::SINGLE_COLOR |   DM::SSE2 | 0*DM::YV12 :
    {
        while(h--)
        {
            const DWORD *sw = switchpts;
            for(int wt=0; wt<w; ++wt)
            {
                // xo is the offset (usually negative) we have moved into the image
                // So if we have passed the switchpoint (?) switch to another colour
                // (So switchpts stores both colours *and* coordinates?)
                if(wt+xo >= sw[1]) {while(wt+xo >= sw[1]) sw += 2; color = sw[-2];}
                pixmix_sse2(&dst[wt], color, (s[wt]*(color>>24))>>8);
            }
            s += overlayPitch;
            dst = (unsigned long *)((char *)dst + spd.pitch);
        }
    }
    break;
    case 0*DM::SINGLE_COLOR | 0*DM::SSE2 | 0*DM::YV12 :
    {
        while(h--)
        {
            const DWORD *sw = switchpts;
            for(int wt=0; wt<w; ++wt)
            {
                if(wt+xo >= sw[1]) {while(wt+xo >= sw[1]) sw += 2; color = sw[-2];}
                pixmix(&dst[wt], color, (s[wt]*(color>>24))>>8);
            }
            s += overlayPitch;
            dst = (unsigned long *)((char *)dst + spd.pitch);
        }
    }
    break;
    case   DM::SINGLE_COLOR |   DM::SSE2 |   DM::YV12 :
    {
        unsigned char* dst_A = (unsigned char*)dst;
        unsigned char* dst_Y = dst_A + spd.pitch*spd.h;
        unsigned char* dst_U = dst_Y + spd.pitch*spd.h;
        unsigned char* dst_V = dst_U + spd.pitch*spd.h;
        while(h--)
        {
            for(int wt=0; wt<w; ++wt)
            {
                DWORD temp = COMBINE_AYUV(dst_A[wt], dst_Y[wt], dst_U[wt], dst_V[wt]);
                pixmix_sse2(&temp, color, s[wt]);
                SPLIT_AYUV(temp, dst_A+wt, dst_Y+wt, dst_U+wt, dst_V+wt);
            }
            s += overlayPitch;
            dst_A += spd.pitch;
            dst_Y += spd.pitch;
            dst_U += spd.pitch;
            dst_V += spd.pitch;
        }
    }
    break;
    case   DM::SINGLE_COLOR | 0*DM::SSE2 |   DM::YV12 :
    {
//        char * debug_dst=(char*)dst;int h2 = h;
//        XY_DO_ONCE( xy_logger::write_file("G:\\b2_rt", (char*)&color, sizeof(color)) );
//        XY_DO_ONCE( xy_logger::write_file("G:\\b2_rt", debug_dst, (h2-1)*spd.pitch) );
//        debug_dst += spd.pitch*spd.h;
//        XY_DO_ONCE( xy_logger::write_file("G:\\b2_rt", debug_dst, (h2-1)*spd.pitch) );
//        debug_dst += spd.pitch*spd.h;
//        XY_DO_ONCE( xy_logger::write_file("G:\\b2_rt", debug_dst, (h2-1)*spd.pitch) );
//        debug_dst += spd.pitch*spd.h;
//        XY_DO_ONCE( xy_logger::write_file("G:\\b2_rt", debug_dst, (h2-1)*spd.pitch) );
//        debug_dst=(char*)dst;

        unsigned char* dst_A = (unsigned char*)dst;
        unsigned char* dst_Y = dst_A + spd.pitch*spd.h;
        unsigned char* dst_U = dst_Y + spd.pitch*spd.h;
        unsigned char* dst_V = dst_U + spd.pitch*spd.h;
        while(h--)
        {
            for(int wt=0; wt<w; ++wt)
            {
                DWORD temp = COMBINE_AYUV(dst_A[wt], dst_Y[wt], dst_U[wt], dst_V[wt]);
                pixmix(&temp, color, s[wt]);
                SPLIT_AYUV(temp, dst_A+wt, dst_Y+wt, dst_U+wt, dst_V+wt);
            }
            s += overlayPitch;
            dst_A += spd.pitch;
            dst_Y += spd.pitch;
            dst_U += spd.pitch;
            dst_V += spd.pitch;
        }
//        XY_DO_ONCE( xy_logger::write_file("G:\\a2_rt", debug_dst, (h2-1)*spd.pitch) );
//        debug_dst += spd.pitch*spd.h;
//        XY_DO_ONCE( xy_logger::write_file("G:\\a2_rt", debug_dst, (h2-1)*spd.pitch) );
//        debug_dst += spd.pitch*spd.h;
//        XY_DO_ONCE( xy_logger::write_file("G:\\a2_rt", debug_dst, (h2-1)*spd.pitch) );
//        debug_dst += spd.pitch*spd.h;
//        XY_DO_ONCE( xy_logger::write_file("G:\\a2_rt", debug_dst, (h2-1)*spd.pitch) );
    }
    break;
    case 0*DM::SINGLE_COLOR |   DM::SSE2 |   DM::YV12 :
    {
        unsigned char* dst_A = (unsigned char*)dst;
        unsigned char* dst_Y = dst_A + spd.pitch*spd.h;
        unsigned char* dst_U = dst_Y + spd.pitch*spd.h;
        unsigned char* dst_V = dst_U + spd.pitch*spd.h;
        while(h--)
        {
            const DWORD *sw = switchpts;
            for(int wt=0; wt<w; ++wt)
            {
                // xo is the offset (usually negative) we have moved into the image
                // So if we have passed the switchpoint (?) switch to another colour
                // (So switchpts stores both colours *and* coordinates?)
                //if(wt+xo >= sw[1]) {while(wt+xo >= sw[1]) sw += 2; color = sw[-2];}
                if(wt+xo >= sw[1]) {while(wt+xo >= sw[1]) sw += 2; color = sw[-2];}
                DWORD temp = COMBINE_AYUV(dst_A[wt], dst_Y[wt], dst_U[wt], dst_V[wt]);
                pixmix_sse2(&temp, color, (s[wt]*(color>>24))>>8);
                SPLIT_AYUV(temp, dst_A+wt, dst_Y+wt, dst_U+wt, dst_V+wt);
            }
            s += overlayPitch;
            dst_A += spd.pitch;
            dst_Y += spd.pitch;
            dst_U += spd.pitch;
            dst_V += spd.pitch;
        }
    }
    break;
    case 0*DM::SINGLE_COLOR | 0*DM::SSE2 |   DM::YV12 :
    {
        unsigned char* dst_A = (unsigned char*)dst;
        unsigned char* dst_Y = dst_A + spd.pitch*spd.h;
        unsigned char* dst_U = dst_Y + spd.pitch*spd.h;
        unsigned char* dst_V = dst_U + spd.pitch*spd.h;
        while(h--)
        {
            const DWORD *sw = switchpts;
            for(int wt=0; wt<w; ++wt)
            {
                if(wt+xo >= sw[1]) {while(wt+xo >= sw[1]) sw += 2; color = sw[-2];}
                DWORD temp = COMBINE_AYUV(dst_A[wt], dst_Y[wt], dst_U[wt], dst_V[wt]);
                pixmix(&temp, color, (s[wt]*(color>>24))>>8);
                SPLIT_AYUV(temp, dst_A+wt, dst_Y+wt, dst_U+wt, dst_V+wt);
            }
            s += overlayPitch;
            dst_A += spd.pitch;
            dst_Y += spd.pitch;
            dst_U += spd.pitch;
            dst_V += spd.pitch;
        }
    }
    break;
    }
    // Remember to EMMS!
    // Rendering fails in funny ways if we don't do this.
    _mm_empty();
    xy_free(s_base);
    return bbox;
}

void Overlay::_DoFillAlphaMash(byte* outputAlphaMask, const byte* pBody, const byte* pBorder, int x, int y, int w, int h, const byte* pAlphaMask, int pitch, DWORD color_alpha )
{
    //    int planSize = mOverlayWidth*mOverlayHeight;
    int x00 = x&~15;
    int w00 = (w+15)&~15;
    //    int x00 = x;
    //    int w00 = w;
    pBody = pBody!=NULL ? pBody + y*mOverlayPitch + x00 : NULL;
    pBorder = pBorder!=NULL ? pBorder + y*mOverlayPitch + x00 : NULL;
    byte* dst = outputAlphaMask + y*mOverlayPitch + x00;

    if(pAlphaMask==NULL && pBody!=NULL && pBorder!=NULL)
    {
        __asm
        {
            mov        eax, color_alpha
                movd	   XMM3, eax
                punpcklwd  XMM3, XMM3
                pshufd	   XMM3, XMM3, 0
        }
        while(h--)
        {
            BYTE* pBorderTmp = (BYTE*)pBorder;
            BYTE* pBodyTmp = (BYTE*)pBody;
            BYTE* dstTmp = (BYTE*)dst;
            for(int j=w00;j>0;j-=16, pBorderTmp+=16, pBodyTmp+=16, dstTmp+=16)
            {
                __asm
                {
                    mov         esi, pBorderTmp
                        movaps      XMM1,[esi]
                    xorps       XMM0,XMM0
                        mov         esi, pBodyTmp
                        movaps      XMM2,[esi]

                    psubusb     XMM1,XMM2

                        movaps      XMM2,XMM1
                        punpcklbw   XMM2,XMM0
                        pmullw      XMM2,XMM3
                        psrlw       XMM2,6

                        mov         edi, dstTmp

                        punpckhbw   XMM1,XMM0
                        pmullw      XMM1,XMM3
                        psrlw       XMM1,6

                        packuswb    XMM2,XMM1
                        movntps     [edi],XMM2
                }
            }
            //            for(int j=0; j<w00; j++)
            //            {
            //                dst[j] = (safe_subtract(pBorder[j], pBody[j]) * color_alpha)>>6;
            //                //dst[j] = safe_subtract(pBorder[j], pBody[j]);
            //            }
            pBorder += mOverlayPitch;
            pBody += mOverlayPitch;
            dst += mOverlayPitch;
        }
    }
    else if( ((pBody==NULL) + (pBorder==NULL))==1 && pAlphaMask==NULL)
    {
        const byte* src1 = pBody!=NULL ? pBody : pBorder;
        while(h--)
        {
            for(int j=0; j<w00; j++)
            {
                dst[j] = (src1[j] * color_alpha)>>6;
                //                dst[j] = (src1[j] * pAlphaMask[j])>>6;
            }
            src1 += mOverlayPitch;
            //            pAlphaMask += pitch;
            dst += mOverlayPitch;
        }
    }
    else if( ((pBody==NULL) + (pBorder==NULL))==1 && pAlphaMask!=NULL)
    {
        const byte* src1 = pBody!=NULL ? pBody : pBorder;
        while(h--)
        {
            for(int j=0; j<w00; j++)
            {
                dst[j] = (src1[j] * pAlphaMask[j] * color_alpha)>>12;
                //                dst[j] = (src1[j] * pAlphaMask[j])>>6;
            }
            src1 += mOverlayPitch;
            pAlphaMask += pitch;
            dst += mOverlayPitch;
        }
    }
    else if( pAlphaMask!=NULL && pBody!=NULL && pBorder!=NULL )
    {
        while(h--)
        {
            for(int j=0; j<w00; j++)
            {
                dst[j] = (safe_subtract(pBorder[j], pBody[j])*pAlphaMask[j]*color_alpha)>>12;
                //                dst[j] = (safe_subtract(pBorder[j], pBody[j])*pAlphaMask[j])>>6;
            }
            pBorder += mOverlayPitch;
            pBody += mOverlayPitch;
            pAlphaMask += pitch;
            dst += mOverlayPitch;
        }
    }
    else
    {
        //should NOT happen!
        ASSERT(0);
    }
}

void Overlay::FillAlphaMash( byte* outputAlphaMask, bool fBody, bool fBorder, int x, int y, int w, int h, const byte* pAlphaMask, int pitch, DWORD color_alpha)
{
    if(!fBorder && fBody && pAlphaMask==NULL)
    {
        _DoFillAlphaMash(outputAlphaMask, mpOverlayBuffer.body, NULL, x, y, w, h, pAlphaMask, pitch, color_alpha);        
    }
    else if(/*fBorder &&*/ fBody && pAlphaMask==NULL)
    {
        _DoFillAlphaMash(outputAlphaMask, NULL, mpOverlayBuffer.border, x, y, w, h, pAlphaMask, pitch, color_alpha);        
    }
    else if(!fBody && fBorder /* pAlphaMask==NULL or not*/)
    {
        _DoFillAlphaMash(outputAlphaMask, mpOverlayBuffer.body, mpOverlayBuffer.border, x, y, w, h, pAlphaMask, pitch, color_alpha);        
    }
    else if(!fBorder && fBody && pAlphaMask!=NULL)
    {
        _DoFillAlphaMash(outputAlphaMask, mpOverlayBuffer.body, NULL, x, y, w, h, pAlphaMask, pitch, color_alpha);        
    }
    else if(fBorder && fBody && pAlphaMask!=NULL)
    {
        _DoFillAlphaMash(outputAlphaMask, NULL, mpOverlayBuffer.border, x, y, w, h, pAlphaMask, pitch, color_alpha);        
    }
    else
    {
        //should NOT happen
        ASSERT(0);
    }
}