/// @copyright (c) 2007 CSIRO
/// Australia Telescope National Facility (ATNF)
/// Commonwealth Scientific and Industrial Research Organisation (CSIRO)
/// PO Box 76, Epping NSW 1710, Australia
/// atnf-enquiries@csiro.au
///
/// This file is part of the ASKAP software distribution.
///
/// The ASKAP software distribution is free software: you can redistribute it
/// and/or modify it under the terms of the GNU General Public License as
/// published by the Free Software Foundation; either version 2 of the License,
/// or (at your option) any later version.
///
/// This program is distributed in the hope that it will be useful,
/// but WITHOUT ANY WARRANTY; without even the implied warranty of
/// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
/// GNU General Public License for more details.
///
/// You should have received a copy of the GNU General Public License
/// along with this program; if not, write to the Free Software
/// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
///
/// @author Ben Humphreys <ben.humphreys@csiro.au>
/// @author Tim Cornwell  <tim.cornwell@csiro.au>

// System includes
#include <iostream>
#include <cmath>
#include <ctime>
#include <complex>
#include <vector>
#include <algorithm>
#include <limits>
#include <cassert>

// OpenMP includes
#include <omp.h>

// Local includes
#include "Stopwatch.h"

// BLAS includes
#ifdef USEBLAS

#define CAXPY cblas_caxpy
#define CDOTU_SUB cblas_cdotu_sub

#include <mkl_cblas.h>

#endif

#if defined(GRIDING)  
	#define SERIAL_GRIDING 1
	#define OMP_GRIDING 1
	#define VERIFY_GRIDING 1
#elif defined(SERIAL_GRIDING)
	#define SERIAL_GRIDING 1
#elif defined(OMP_GRIDING)
	#define OMP_GRIDING 1


#elif defined(DEGRIDING)
	#define SERIAL_DEGRIDING 1
	#define OMP_DEGRIDING 1
	#define VERIFY_DEGRIDING 1
#elif defined(SERIAL_DEGRIDING)
	#define SERIAL_DEGRIDING 1
#elif defined(OMP_DEGRIDING)
	#define OMP_DEGRIDING 1
#else
	#define SERIAL_GRIDING 1
	#define OMP_GRIDING 1
	#define VERIFY_GRIDING 1

	#define SERIAL_DEGRIDING 1
	#define OMP_DEGRIDING 1
	#define VERIFY_DEGRIDING 1
#endif  

using std::cout;
using std::endl;
using std::abs;

// Typedefs for easy testing
// Cost of using double for Coord is low, cost for
// double for Real is also low
typedef double Coord;
typedef float Real;
typedef std::complex<Real> Value;


/////////////////////////////////////////////////////////////////////////////////
// The next two functions are the kernel of the gridding/degridding.
// The data are presented as a vector. Offsets for the convolution function
// and for the grid location are precalculated so that the kernel does
// not need to know anything about world coordinates or the shape of
// the convolution function. The ordering of cOffset and iu, iv is
// random - some presorting might be advantageous.
//
// Perform gridding
//
// data - values to be gridded in a 1D vector
// support - Total width of convolution function=2*support+1
// C - convolution function shape: (2*support+1, 2*support+1, *)
// cOffset - offset into convolution function per data point
// iu, iv - integer locations of grid points
// grid - Output grid: shape (gSize, *)
// gSize - size of one axis of grid
void gridKernel(const std::vector<Value>& data, const int support,
                const std::vector<Value>& C, const std::vector<int>& cOffset,
                const std::vector<int>& iu, const std::vector<int>& iv,
                std::vector<Value>& grid, const int gSize)
{
    const int sSize = 2 * support + 1;

    for (int dind = 0; dind < int(data.size()); ++dind) {
        // The actual grid point
        int gind = iu[dind] + gSize * iv[dind] - support;
        // The Convoluton function point from which we offset
        int cind = cOffset[dind];

        for (int suppv = 0; suppv < sSize; suppv++) {
#ifdef USEBLAS
            CAXPY(sSize, &data[dind], &C[cind], 1, &grid[gind], 1);
#else
            Value* gptr = &grid[gind];
            const Value* cptr = &C[cind];
            const Value d = data[dind];

            for (int suppu = 0; suppu < sSize; suppu++) {
                *(gptr++) += d * (*(cptr++));
            }

#endif
            gind += gSize;
            cind += sSize;
        }
    }
}

#pragma GCC optimize("prefetch-loop-arrays")
int gridKernelOMP(const std::vector<Value>& data, const int support,
        const std::vector<Value>& C, const std::vector<int>& cOffset,
        const std::vector<int>& iu, const std::vector<int>& iv,
        std::vector<Value>& grid, const int gSize)
{
    const int sSize = 2 * support + 1;
    #pragma omp parallel default(shared)
    {
        const int tid = omp_get_thread_num();
        const int nthreads = omp_get_num_threads();

        for (int dind = 0; dind < int(data.size()); ++dind) {
            // The actual grid point
            int gind = iu[dind] + gSize * iv[dind] - support;
            // The Convoluton function point from which we offset
            int cind = cOffset[dind];
            int row = iv[dind] % nthreads;
            for (int suppv = 0; suppv < sSize; suppv++) {
                if (row == tid) {
#ifdef USEBLAS
                    CAXPY(sSize, &data[dind], &C[cind], 1, &grid[gind], 1);
#else
                    Value* gptr = &grid[gind];
                    const Value* cptr = &C[cind];
                    const Value d = data[dind];

                    for (int suppu = 0; suppu < sSize; suppu++) {
                        *(gptr++) += d * (*(cptr++));
                    }
#endif
                }
                gind += gSize;
                cind += sSize;
                row++;
                row = (row >= nthreads) ? 0 : row;
            }
        }
    } // End omp parallel

    return omp_get_max_threads();
}
#pragma GCC optimize("no-prefetch-loop-arrays")

// Perform degridding
void degridKernel(const std::vector<Value>& grid, const int gSize, const int support,
                  const std::vector<Value>& C, const std::vector<int>& cOffset,
                  const std::vector<int>& iu, const std::vector<int>& iv,
                  std::vector<Value>& data)
{
    const int sSize = 2 * support + 1;

    for (int dind = 0; dind < int(data.size()); ++dind) {
        data[dind] = 0.0;

        // The actual grid point from which we offset
        int gind = iu[dind] + gSize * iv[dind] - support;
        // The Convoluton function point from which we offset
        int cind = cOffset[dind];

        for (int suppv = 0; suppv < sSize; suppv++) {
#ifdef USEBLAS
            Value dot;
            CDOTU_SUB(sSize, &grid[gind], 1, &C[cind], 1, &dot);
            data[dind] += dot;
#else
            Value* d = &data[dind];
            const Value* gptr = &grid[gind];
            const Value* cptr = &C[cind];

            for (int suppu = 0; suppu < sSize; suppu++) {
                (*d) += (*(gptr++)) * (*(cptr++));
            }

#endif
            gind += gSize;
            cind += sSize;
        }

    }
}

int degridKernelOMP(const std::vector<Value>& grid, const int gSize, const int support,
                    const std::vector<Value>& C, const std::vector<int>& cOffset,
                    const std::vector<int>& iu, const std::vector<int>& iv,
                    std::vector<Value>& data)
{
    const int sSize = 2 * support + 1;

    #pragma omp parallel for  \
        default(shared)   \
        schedule(dynamic, 32)
    for (int dind = 0; dind < int(data.size()); ++dind) {
        data[dind] = 0.0;

        // The actual grid point from which we offset
        int gind = iu[dind] + gSize * iv[dind] - support;
        // The Convoluton function point from which we offset
        int cind = cOffset[dind];

        for (int suppv = 0; suppv < sSize; suppv++) {
#ifdef USEBLAS
            Value dot;
            CDOTU_SUB(sSize, &grid[gind], 1, &C[cind], 1, &dot);
            data[dind] += dot;
#else
            Value* d = &data[dind];
            const Value* gptr = &grid[gind];
            const Value* cptr = &C[cind];

            for (int suppu = 0; suppu < sSize; suppu++) {
                (*d) += (*(gptr++)) * (*(cptr++));
            }

#endif
            gind += gSize;
            cind += sSize;
        }

    }

    return omp_get_max_threads();
}

/////////////////////////////////////////////////////////////////////////////////
// Initialize W project convolution function
// - This is application specific and should not need any changes.
//
// freq - temporal frequency (inverse wavelengths)
// cellSize - size of one grid cell in wavelengths
// support - Total width of convolution function=2*support+1
// wCellSize - size of one w grid cell in wavelengths
// wSize - Size of lookup table in w
void initC(const std::vector<Coord>& freq, const Coord cellSize,
           const Coord baseline,
           const int wSize, int& support, int& overSample,
           Coord& wCellSize, std::vector<Value>& C)
{
    cout << "Initializing W projection convolution function" << endl;
    // DAM -- I don't really understand the following equation. baseline*freq is the array size in wavelengths,
    // but I don't know why the sqrt is used and why there is a multiplication with cellSize rather than a division.
    // In the paper referred to in ../README.md they suggest using rms(w)*FoV for the width (in wavelengths), which
    // would lead to something more like:
    // support = max( 3, ceil( 0.5 * scale*baseline*freq[0] / (cellSize*cellSize) ) )
    // where "scale" reduces the maximum baseline length to the RMS (1/sqrt(3) for uniformaly distributed
    // visibilities, 1/(2+log10(n)/2) or so for n baselines with a Gaussian radial profile).
    support = static_cast<int>(1.5 * sqrt(std::abs(baseline) * static_cast<Coord>(cellSize)
                                          * freq[0]) / cellSize);

    cout << "FoV = " << 180./3.14159265/cellSize << " deg" << endl;

    overSample = 8;
    cout << "Support = " << support << " pixels" << endl;
    wCellSize = 2 * baseline * freq[0] / wSize;
    cout << "W cellsize = " << wCellSize << " wavelengths" << endl;

    // Convolution function. This should be the convolution of the
    // w projection kernel (the Fresnel term) with the convolution
    // function used in the standard case. The latter is needed to
    // suppress aliasing. In practice, we calculate entire function
    // by Fourier transformation. Here we take an approximation that
    // is good enough.
    const int sSize = 2 * support + 1;

    const int cCenter = (sSize - 1) / 2;

    C.resize(sSize*sSize*overSample*overSample*wSize);
    cout << "Size of convolution function = " << sSize*sSize*overSample
         *overSample*wSize*sizeof(Value) / (1024*1024) << " MB" << std::endl;
    cout << "Shape of convolution function = [" << sSize << ", " << sSize << ", "
             << overSample << ", " << overSample << ", " << wSize << "]" << std::endl;

    for (int k = 0; k < wSize; k++) {
        double w = double(k - wSize / 2);
        double fScale = sqrt(abs(w) * wCellSize * freq[0]) / cellSize;

        for (int osj = 0; osj < overSample; osj++) {
            for (int osi = 0; osi < overSample; osi++) {
                for (int j = 0; j < sSize; j++) {
                    const double j2 = std::pow((double(j - cCenter) + double(osj) / double(overSample)), 2);

                    for (int i = 0; i < sSize; i++) {
                        const double r2 = j2 + std::pow((double(i - cCenter) + double(osi) / double(overSample)), 2);
                        const int cind = i + sSize * (j + sSize * (osi + overSample * (osj + overSample * k)));

                        if (w != 0.0) {
                            C[cind] = static_cast<Value>(std::cos(r2 / (w * fScale)));
                        } else {
                            C[cind] = static_cast<Value>(std::exp(-r2));
                        }
                    }
                }
            }
        }
    }

    // Now normalise the convolution function
    Real sumC = 0.0;

    for (int i = 0; i < sSize*sSize*overSample*overSample*wSize; i++) {
        sumC += abs(C[i]);
    }

    for (int i = 0; i < sSize*sSize*overSample*overSample*wSize; i++) {
        C[i] *= Value(wSize * overSample * overSample / sumC);
    }
}

// Initialize Lookup function
// - This is application specific and should not need any changes.
//
// freq - temporal frequency (inverse wavelengths)
// cellSize - size of one grid cell in wavelengths
// gSize - size of grid in pixels (per axis)
// support - Total width of convolution function=2*support+1
// wCellSize - size of one w grid cell in wavelengths
// wSize - Size of lookup table in w
void initCOffset(const std::vector<Coord>& u, const std::vector<Coord>& v,
                 const std::vector<Coord>& w, const std::vector<Coord>& freq,
                 const Coord cellSize, const Coord wCellSize,
                 const int wSize, const int gSize, const int support, const int overSample,
                 std::vector<int>& cOffset, std::vector<int>& iu,
                 std::vector<int>& iv)
{
    const int nSamples = u.size();
    const int nChan = freq.size();

    const int sSize = 2 * support + 1;

    // Now calculate the offset for each visibility point
    cOffset.resize(nSamples*nChan);
    iu.resize(nSamples*nChan);
    iv.resize(nSamples*nChan);

    for (int i = 0; i < nSamples; i++) {
        for (int chan = 0; chan < nChan; chan++) {

            const int dind = i * nChan + chan;

            const Coord uScaled = freq[chan] * u[i] / cellSize;
            iu[dind] = int(uScaled);

            if (uScaled < Coord(iu[dind])) {
                iu[dind] -= 1;
            }

            const int fracu = int(overSample * (uScaled - Coord(iu[dind])));
            iu[dind] += gSize / 2;

            const Coord vScaled = freq[chan] * v[i] / cellSize;
            iv[dind] = int(vScaled);

            if (vScaled < Coord(iv[dind])) {
                iv[dind] -= 1;
            }

            const int fracv = int(overSample * (vScaled - Coord(iv[dind])));
            iv[dind] += gSize / 2;

            // The beginning of the convolution function for this point
            Coord wScaled = freq[chan] * w[i] / wCellSize;
            int woff = wSize / 2 + int(wScaled);
            cOffset[dind] = sSize * sSize * (fracu + overSample * (fracv + overSample * woff));
        }
    }

}

// Return a pseudo-random integer in the range 0..2147483647
// Based on an algorithm in Kernighan & Ritchie, "The C Programming Language"
static unsigned long next = 1;
int randomInt()
{
    const unsigned int maxint = std::numeric_limits<int>::max();
    next = next * 1103515245 + 12345;
    return ((unsigned int)(next / 65536) % maxint);
}

void usage() {
    cout << "usage: tConvolveOMP [-h] [option]" << endl;
    cout << "-n num\t change the number of data samples to num." << endl;
    cout << "-w num\t change the number of lookup planes in w projection to num." << endl;
    cout << "-c num\t change the number of spectral channels to num." << endl;
    cout << "-f val\t reduce the field of view by a factor of val (=> reduce the kernel size)." << endl;
}

// Main testing routine
int main(int argc, char* argv[])
{
    // Change these if necessary to adjust run time
    int nSamples = 160000; // Number of data samples
    int wSize = 33; // Number of lookup planes in w projection
    int nChan = 1; // Number of spectral channels
    Coord cellSize = 5.0; // Cellsize of output grid in wavelengths

    if (argc > 1) {
        for (int i=1; i < argc; i++) {
            if (argv[i][0] == '-') {
                if (argv[i][1] == 'h') {
                    usage();
                    return 0;
                }
                else if (argv[i][1] == 'n') {
                    nSamples = atoi(argv[i+1]);
                    i++;
                }
                else if (argv[i][1] == 'w') {
                    wSize = atoi(argv[i+1]);
                    i++;
                }
                else if (argv[i][1] == 'c') {
                    nChan = atoi(argv[i+1]);
                    i++;
                }
                else if (argv[i][1] == 'f') {
                    cellSize *= atof(argv[i+1]);
                    i++;
                }
                else {
                    usage();
                    return 1;
                }
            }
            else {
                usage();
                return 1;
            }
        }
    }
    // Don't change any of these numbers unless you know what you are doing!
    const int gSize = 4096; // Size of output grid in pixels
    const int baseline = 2000; // Maximum baseline in meters

    cout << "nSamples = " << nSamples <<endl;
    // Initialize the data to be gridded
    std::vector<Coord> u(nSamples);
    std::vector<Coord> v(nSamples);
    std::vector<Coord> w(nSamples);
    std::vector<Value> data(nSamples*nChan);
    std::vector<Value> cpuoutdata(nSamples*nChan);
    std::vector<Value> ompoutdata(nSamples*nChan);

    const unsigned int maxint = std::numeric_limits<int>::max();

    for (int i = 0; i < nSamples; i++) {
        u[i] = baseline * Coord(randomInt()) / Coord(maxint) - baseline / 2;
        v[i] = baseline * Coord(randomInt()) / Coord(maxint) - baseline / 2;
        w[i] = baseline * Coord(randomInt()) / Coord(maxint) - baseline / 2;

        for (int chan = 0; chan < nChan; chan++) {
            data[i*nChan+chan] = 1.0;
            cpuoutdata[i*nChan+chan] = 0.0;
            ompoutdata[i*nChan+chan] = 0.0;
        }
    }

    std::vector<Value> grid(gSize*gSize);
    grid.assign(grid.size(), Value(0.0));

    // Measure frequency in inverse wavelengths
    std::vector<Coord> freq(nChan);

    for (int i = 0; i < nChan; i++) {
        freq[i] = (1.4e9 - 2.0e5 * Coord(i) / Coord(nChan)) / 2.998e8;
    }

    // Initialize convolution function and offsets
    std::vector<Value> C;
    int support, overSample;
    std::vector<int> cOffset;
    // Vectors of grid centers
    std::vector<int> iu;
    std::vector<int> iv;
    Coord wCellSize;

    initC(freq, cellSize, baseline, wSize, support, overSample, wCellSize, C);
    initCOffset(u, v, w, freq, cellSize, wCellSize, wSize, gSize, support,
                overSample, cOffset, iu, iv);
    const int sSize = 2 * support + 1;

    const double griddings = (double(nSamples * nChan) * double((sSize) * (sSize)));


    ///////////////////////////////////////////////////////////////////////////
    // DO GRIDDING
    ///////////////////////////////////////////////////////////////////////////
    std::vector<Value> cpugrid(gSize*gSize);
#ifdef SERIAL_GRIDING
    cpugrid.assign(cpugrid.size(), Value(0.0));
    {
        // Now we can do the timing for the CPU implementation
        cout << "+++++ Forward processing (CPU) +++++" << endl;

        Stopwatch sw;
        sw.start();
        gridKernel(data, support, C, cOffset, iu, iv, cpugrid, gSize);
        double time = sw.stop();

        // Report on timings
        cout << "    Time " << time << " (s) " << endl;
        cout << "    Time per visibility spectral sample " << 1e6*time / double(data.size()) << " (us) " << endl;
        cout << "    Time per gridding   " << 1e9*time / (double(data.size())* double((sSize)*(sSize))) << " (ns) " << endl;
        cout << "    Gridding rate   " << (griddings / 1000000) / time << " (million grid points per second)" << endl;

        cout << "Done" << endl;
    }
#endif

    std::vector<Value> ompgrid(gSize*gSize);
#ifdef OMP_GRIDING
    ompgrid.assign(ompgrid.size(), Value(0.0));
    {
        // Now we can do the timing for the GPU implementation
        cout << "+++++ Forward processing (OpenMP) +++++" << endl;

        // Time is measured inside this function call, unlike the CPU versions
        Stopwatch sw;
        sw.start();
        const int nthreads = gridKernelOMP(data, support, C, cOffset, iu, iv, ompgrid, gSize);
        const double time = sw.stop();

        // Report on timings
        cout << "    Num threads: " << nthreads << endl;
        cout << "    Time " << time << " (s) " << endl;
        cout << "    Time per visibility spectral sample " << 1e6*time / double(data.size()) << " (us) " << endl;
        cout << "    Time per gridding   " << 1e9*time / (double(data.size())* double((sSize)*(sSize))) << " (ns) " << endl;
        cout << "    Gridding rate   " << (griddings / 1000000) / time << " (million grid points per second)" << endl;

        cout << "Done" << endl;
    }
#endif

#ifdef VERIFY_GRIDING
    cout << "Verifying result...";

    if (cpugrid.size() != ompgrid.size()) {
        cout << "Fail (Grid sizes differ)" << std::endl;
        return 1;
    }

    for (unsigned int i = 0; i < cpugrid.size(); ++i) {
        if (fabs(cpugrid[i].real() - ompgrid[i].real()) > 0.00001) {
            cout << "Fail (Expected " << cpugrid[i].real() << " got "
                     << ompgrid[i].real() << " at index " << i << ")"
                     << std::endl;
            return 1;
        }
    }

    cout << "Pass" << std::endl;
#endif

    ///////////////////////////////////////////////////////////////////////////
    // DO DEGRIDDING
    ///////////////////////////////////////////////////////////////////////////
#ifdef SERIAL_DEGRIDING
    {
        cpugrid.assign(cpugrid.size(), Value(1.0));
        // Now we can do the timing for the CPU implementation
        cout << "+++++ Reverse processing (CPU) +++++" << endl;

        Stopwatch sw;
        sw.start();
        degridKernel(cpugrid, gSize, support, C, cOffset, iu, iv, cpuoutdata);
        const double time = sw.stop();

        // Report on timings
        cout << "    Time " << time << " (s) " << endl;
        cout << "    Time per visibility spectral sample " << 1e6*time / double(data.size()) << " (us) " << endl;
        cout << "    Time per degridding   " << 1e9*time / (double(data.size())* double((sSize)*(sSize))) << " (ns) " << endl;
        cout << "    Degridding rate   " << (griddings / 1000000) / time << " (million grid points per second)" << endl;

        cout << "Done" << endl;
    }
#endif
#ifdef OMP_DEGRIDING
    {
        ompgrid.assign(ompgrid.size(), Value(1.0));
        // Now we can do the timing for the GPU implementation
        cout << "+++++ Reverse processing (OpenMP) +++++" << endl;

        // Time is measured inside this function call, unlike the CPU versions
        Stopwatch sw;
        sw.start();
        const int nthreads = degridKernelOMP(ompgrid, gSize, support, C, cOffset, iu, iv, ompoutdata);
        const double time = sw.stop();

        // Report on timings
        cout << "    Num threads: " << nthreads << endl;
        cout << "    Time " << time << " (s) " << endl;
        cout << "    Time per visibility spectral sample " << 1e6*time / double(data.size()) << " (us) " << endl;
        cout << "    Time per degridding   " << 1e9*time / (double(data.size())* double((sSize)*(sSize))) << " (ns) " << endl;
        cout << "    Degridding rate   " << (griddings / 1000000) / time << " (million grid points per second)" << endl;

        cout << "Done" << endl;
    }
#endif

#ifdef VERIFY_DEGRIDING
    // Verify degridding results
    cout << "Verifying result...";

    if (cpuoutdata.size() != ompoutdata.size()) {
        cout << "Fail (Data vector sizes differ)" << std::endl;
        return 1;
    }

    for (unsigned int i = 0; i < cpuoutdata.size(); ++i) {
        if (fabs(cpuoutdata[i].real() - ompoutdata[i].real()) > 0.00001) {
            cout << "Fail (Expected " << cpuoutdata[i].real() << " got "
                     << ompoutdata[i].real() << " at index " << i << ")"
                     << std::endl;
            return 1;
        }
    }

    cout << "Pass" << std::endl;

    return 0;
#endif
}
