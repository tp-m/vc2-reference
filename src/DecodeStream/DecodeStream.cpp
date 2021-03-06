/***********************************************************************/
/* DecodeStream.cpp                                                    */
/* Author: James Weaver                                                */
/* This version 18th June 2015                                         */
/*                                                                     */
/* Reads compressed stream in                                          */
/* Decompresses image using VC-2                                       */
/* Writes image data out to a planar file.                             */
/* It is not necessarily complet nor korrect.                          */
/* Copyright (c) BBC 2011-2015 -- For license see the LICENSE file   */
/***********************************************************************/

const char version[] = __DATE__ " @ " __TIME__ ;
const char summary[] = "Decodes a VC-2 stream to an uncompressed planar file";
const char description[] = "\
This program decodes SMPTE VC-2 stream data to regenerate an image sequence.\n\
Its primary output is the decoded image sequence. However it may produce alternative outputs which are:\n\
  1 the wavelet transform of the decoded output (inverse quantised wavelet coefficients)\n\
  2 the quantised wavelet coefficients\n\
  3 the quantisation indices used for each slice\n\
  4 the decoded sequence\n\
Input is a VC-2 stream.\n\
Output (where appropriate) are in planar format (4:4:4, 4:2:2, 4:2:0 or RGB).\n\
There can be 1 to 4 bytes per sample and the data is left (MSB) justified.\n\
Data is assumed offset binary (which is fine for both YCbCr or RGB).\n\
\n\
Example: DecodeStream -v inFileName outFileName";
const char* details[] = {version, summary, description};

#include <cstdlib> //for EXIT_SUCCESS, EXIT_FAILURE, atoi
#include <stdexcept> //For standard logic errors
#include <iostream> //For cin, cout, cerr
#include <string>
#include <fstream>
#include <cstdio> // for perror
#include <boost/scoped_ptr.hpp>

#include "DecodeParams.h"
#include "Arrays.h"
#include "Slices.h"
#include "Picture.h"
#include "Frame.h"
#include "Quantisation.h"
#include "WaveletTransform.h"
#include "Utils.h"
#include "DataUnit.h"

using std::cout;
using std::cin;
using std::cerr;
using std::clog;
using std::endl;
using std::string;
using std::filebuf;
using std::streambuf;
using std::ios_base;
using std::istream;
using std::ostream;

int main(int argc, char * argv[]) {
  
try { //Giant try block around all code to get error messages

  ProgramParams params = getCommandLineParams(argc, argv, details);
  if (!params.error.empty()) {
    cerr << "Command line error: " << params.error << endl;
    return EXIT_FAILURE;
  }

  // Create convenient aliases for program parameters
  const string inFileName = params.inFileName;
  const string outFileName = params.outFileName;
  const bool verbose = params.verbose;
  const Output output = params.output;

  if (verbose) {
    clog << endl;
    for (int arg=0; arg<argc; ++arg) { //Output command line
      if (arg) clog << " ";
      clog << argv[arg];
    }
    clog << endl;
    clog << "input file = " << inFileName << endl;
    clog << "output file = " << outFileName << endl;
  }

  // Open input file or use standard input
  // Input stream is read only binary mode.
  // No point in continuing if can't open input file.
  filebuf inFileBuffer; // For file input. Needs to be defined here to remain in scope
  streambuf *pInBuffer; // Either standard input buffer or a file buffer
  if (inFileName=="-") { // Use standard in
    //Set standard input to binary mode.
    //Only relevant for Windows (*nix is always binary)
    if ( utils::setstdinmode(std::ios_base::binary) == -1 ) {
        cerr << "Error: could not set standard input to binary mode" << endl;
        return EXIT_FAILURE;
    }
    pInBuffer = cin.rdbuf();
  }
  else { // Open file inFileName and use it for input
    pInBuffer = inFileBuffer.open(inFileName.c_str(), ios_base::in|ios_base::binary);
    if (!pInBuffer) {
      perror((string("Failed to open input file \"")+inFileName+"\"").c_str());
      return EXIT_FAILURE;
    }
  }
  istream inStream(pInBuffer);

  // Open output file or use standard output.
  // Output stream is write only binary mode
  // No point in continuing if can't open output file.
  filebuf outFileBuffer; // For file output. Needs to be defined here to remain in scope
  streambuf *pOutBuffer; // Either standard output buffer or a file buffer
  if (outFileName=="-") { // Use standard out
    //Set standard input to binary mode.
    //Only relevant for Windows (*nix is always binary)
    if ( utils::setstdoutmode(std::ios_base::binary) == -1 ) {
        cerr << "Error: could not set standard output to binary mode" << endl;
        return EXIT_FAILURE;
    }
    pOutBuffer = cout.rdbuf();
  }
  else { // Open file outFileName and use it for output
    pOutBuffer = outFileBuffer.open(outFileName.c_str(), ios_base::out|ios_base::binary);
    if (!pOutBuffer) {
      perror((string("Failed to open output file \"")+outFileName+"\"").c_str());
      return EXIT_FAILURE;
    }
  }
  ostream outStream(pOutBuffer);


  int frame = 0;
  int pic = 0;
  inStream >> dataunitio::synchronise;


  /* In reality the decode can't happen without these values being set, but
     the compiler doesn't like leaving them uninitialised. */
  bool have_seq_hdr         = false;
  int height                = 0;
  int width                 = 0;
  ColourFormat chromaFormat = UNKNOWN;
  int bytes                 = 0;
  int lumaDepth             = 0;
  int chromaDepth           = 0;
  bool interlaced           = false;
  bool topFieldFirst        = false;
  WaveletKernel kernel;
  int waveletDepth;
  int ySlices;
  int xSlices;
  int compressedBytes;
  int sliceScalar;
  boost::scoped_ptr<Frame> outFrame;
  
  while (true) {
    // Read data unit from stream
    if (!inStream) {
      // TODO: Add proper handling
      break;
    }

    DataUnit du;
    inStream >> du;

    if (verbose) {
      clog << endl;
      clog << "Have read data unit of type: " << du.type << endl;
    }

    switch (du.type) {
    case SEQUENCE_HEADER:
      {
        if (verbose) clog << "Parsing Sequence Header" << endl << endl;

        SequenceHeader seq_hdr;
        du.stream() >> seq_hdr;

        if (verbose) {
          clog << "height        = " << seq_hdr.height << endl;
          clog << "width         = " << seq_hdr.width << endl;
          clog << "chroma format = " << seq_hdr.chromaFormat << endl;
          clog << "interlaced    = " << std::boolalpha << seq_hdr.interlace << endl;
          clog << "frame rate    = " << seq_hdr.frameRate << endl;
        }

        height        = seq_hdr.height;
        width         = seq_hdr.width;
        chromaFormat  = seq_hdr.chromaFormat;
        interlaced    = seq_hdr.interlace;
        topFieldFirst = seq_hdr.topFieldFirst;
        lumaDepth     = seq_hdr.bitdepth;
        chromaDepth   = seq_hdr.bitdepth;

        if (seq_hdr.bitdepth == 8)
          bytes = 1;
        else
          bytes = 2;

        have_seq_hdr = true;
      }
      break;
    case END_OF_SEQUENCE:
      if (verbose) {
        clog << "End of Sequence after " << frame << " frames, exiting" << endl;
        return EXIT_SUCCESS;
      }
    case LD_PICTURE:
      {
        if (verbose) clog << "Parsing Picture Header" << endl;

        PicturePreamble preamble;
        du.stream() >> dataunitio::lowDelay
                    >> preamble;

        if (verbose) {
          clog << "Picture number      : " << preamble.picture_number << endl;
          clog << "Wavelet Kernel      : " << preamble.wavelet_kernel << endl;
          clog << "Transform Depth     : " << preamble.depth << endl;
          clog << "Slices Horizontally : " << preamble.slices_x << endl;
          clog << "Slices Verically    : " << preamble.slices_y << endl;
          clog << "Slice Bytes         : " << preamble.slice_bytes << endl;
        }

        ySlices = preamble.slices_y;
        xSlices = preamble.slices_x;
        waveletDepth = preamble.depth;
        kernel = preamble.wavelet_kernel;
        compressedBytes = (preamble.slice_bytes.numerator*preamble.slices_y*preamble.slices_x)/preamble.slice_bytes.denominator;
      }

      if (!have_seq_hdr) {
        clog << "Cannot decode frame, no previous sequence header!" << endl;
      } else {
        // Calculate number of slices per picture
        const int pictureHeight = ( (interlaced) ? height/2 : height);
        const int paddedPictureHeight = paddedSize(pictureHeight, waveletDepth);
        const int paddedWidth = paddedSize(width, waveletDepth);

        // Calculate the quantisation matrix
        const Array1D qMatrix = quantMatrix(kernel, waveletDepth);
        if (verbose) {
          clog << "Quantisation matrix = " << qMatrix[0];
          for (unsigned int i=1; i<qMatrix.size(); ++i) {
            clog << ", " << qMatrix[i];
          }
          clog << endl;
        }

        // First calculate number of bytes for each slice
        const int pictureBytes = (interlaced ? compressedBytes/2 : compressedBytes);
        const Array2D sliceBytes = slice_bytes(ySlices, xSlices, pictureBytes, 1);
        const PictureFormat transformFormat(paddedPictureHeight, paddedWidth, chromaFormat);
        Slices inSlices(transformFormat, waveletDepth, ySlices, xSlices);

        // Define picture format (field or frame)
        const PictureFormat picFormat(pictureHeight, width, chromaFormat);

        // Read input from planar file
        if (verbose) {
          if (interlaced)
            clog << "Reading compressed input field " << pic << " of frame " << frame;
          else
            clog << "Reading compressed input frame number " << frame;
        }
        clog.flush(); // Make sure comments written to log file.
        du.stream() >> sliceio::lowDelay(sliceBytes); // Read input in Low Delay mode
        du.stream() >> inSlices; // Read the compressed input picture
        // Check picture was read OK
        if (!du.stream()) {
          cerr << "\rFailed to read compressed frame" << endl;
          continue;
        }
        else clog << endl;
    
        // Reorder quantised coefficients from slice order to transform order
        if (verbose) clog << "Merge slices into full picture" << endl;
        const Picture yuvQCoeffs = merge_blocks(inSlices.yuvSlices);

        if (output==INDICES) {
          //Write quantisation indices as 1 byte unsigned values
          clog << "Writing quantisation indices to output file" << endl;
          outStream << arrayio::wordWidth(1); //1 byte per sample
          outStream << arrayio::unsigned_binary; // unsigned output
          outStream << inSlices.qIndices;
          if (!outStream) {
            cerr << "Failed to write output file \"" << outFileName << "\"" << endl;
            return EXIT_FAILURE; }
          continue; // omit rest of processing for this picture
        }
      
        if (output==QUANTISED) {
          //Write quantised transform output as 4 byte 2's comp values
          clog << "Writing quantised transform coefficients to output file" << endl;
          outStream << pictureio::wordWidth(4); // 4 bytes per sample
          outStream << pictureio::signed_binary; // 2's comp output
          outStream << yuvQCoeffs;
          if (!outStream) {
            cerr << "Failed to write output file \"" << outFileName << "\"" << endl;
            return EXIT_FAILURE; }
          continue; // omit rest of processing for this picture
        }
    
        // Inverse quantise in transform order
        if (verbose) clog << "Inverse quantise" << endl;
        const Picture yuvTransform = inverse_quantise_transform(yuvQCoeffs, inSlices.qIndices, qMatrix);

        if (output==TRANSFORM) {
          //Write transform output as 4 byte 2's comp values
          clog << "Writing transform coefficients to output file" << endl;
          outStream << pictureio::wordWidth(4); //4 bytes per sample
          outStream << pictureio::signed_binary;
          outStream << yuvTransform;
          if (!outStream) {
            cerr << "Failed to write output file \"" << outFileName << "\"" << endl;
            return EXIT_FAILURE; }
          continue; // omit rest of processing for this picture
        }

        // Inverse wavelet transform
        if (verbose) clog << "Inverse transform" << endl;
        const Picture outPicture = inverseWaveletTransform(yuvTransform, kernel, waveletDepth, picFormat);

        // Copy picture to output frame
        if (verbose) clog << "Copy picture to output frame" << endl;
        if (interlaced) {
          if (pic == 0) {
            const PictureFormat frameFormat(height, width, chromaFormat);
            outFrame.reset(new Frame(frameFormat, interlaced, topFieldFirst));
            outFrame->firstField(outPicture);
              
            pic++;
            continue;
          }

          outFrame->secondField(outPicture);
          pic = 0;
        }
        else { //progressive
          outFrame->frame(outPicture);
        }

        if (verbose) clog << "Clipping output" << endl;
        {
          const int yMin = -utils::pow(2, lumaDepth-1);
          const int yMax = utils::pow(2, lumaDepth-1)-1;
          const int uvMin = -utils::pow(2, chromaDepth-1);
          const int uvMax = utils::pow(2, chromaDepth-1)-1;
          outFrame->frame(clip(*outFrame, yMin, yMax, uvMin, uvMax));
        }

        if (verbose) clog << "Writing decoded output file" << endl;
        outStream << pictureio::wordWidth(bytes); // Set number of bytes per value in file
        outStream << pictureio::left_justified;
        outStream << pictureio::offset_binary;
        outStream << pictureio::bitDepth(lumaDepth, chromaDepth); // Set luma and chroma bit depths
        outStream << *outFrame;
        if (!outStream) {
          cerr << "Failed to write output file \"" << outFileName << "\"" << endl;
          return EXIT_FAILURE;
        }

        ++frame;
      }
      break;
    case HQ_PICTURE:
      {
        if (verbose) clog << "Parsing Picture Header" << endl;

        PicturePreamble preamble;
        du.stream() >> dataunitio::highQualityVBR(1)
                    >> preamble;

        if (verbose) {
          clog << "Picture number      : " << preamble.picture_number << endl;
          clog << "Wavelet Kernel      : " << preamble.wavelet_kernel << endl;
          clog << "Transform Depth     : " << preamble.depth << endl;
          clog << "Slices Horizontally : " << preamble.slices_x << endl;
          clog << "Slices Verically    : " << preamble.slices_y << endl;
          clog << "Slice Prefix        : " << preamble.slice_prefix << endl;
          clog << "Slice Size Scalar   : " << preamble.slice_size_scalar << endl;
        }

        ySlices = preamble.slices_y;
        xSlices = preamble.slices_x;
        waveletDepth = preamble.depth;
        kernel = preamble.wavelet_kernel;
        sliceScalar = preamble.slice_size_scalar;
      }

      if (!have_seq_hdr) {
        clog << "Cannot decode frame, no previous sequence header!" << endl;
      } else {
        // Calculate number of slices per picture
        const int pictureHeight = ( (interlaced) ? height/2 : height);
        const int paddedPictureHeight = paddedSize(pictureHeight, waveletDepth);
        const int paddedWidth = paddedSize(width, waveletDepth);

        // Calculate the quantisation matrix
        const Array1D qMatrix = quantMatrix(kernel, waveletDepth);
        if (verbose) {
          clog << "Quantisation matrix = " << qMatrix[0];
          for (unsigned int i=1; i<qMatrix.size(); ++i) {
            clog << ", " << qMatrix[i];
          }
          clog << endl;
        }

        // Construct an container to read the compressed data into.
        const PictureFormat transformFormat(paddedPictureHeight, paddedWidth, chromaFormat);
        Slices inSlices(transformFormat, waveletDepth, ySlices, xSlices);

        // Define picture format (field or frame)
        const PictureFormat picFormat(pictureHeight, width, chromaFormat);


        // Read input from planar file
        if (verbose) {
          if (interlaced)
            clog << "Reading compressed input field " << pic << " of frame " << frame;
          else
            clog << "Reading compressed input frame number " << frame;
        }
        clog.flush(); // Make sure comments written to log file.
        du.stream() >> sliceio::highQualityVBR(sliceScalar); // Read input in HQ VBR mode
        du.stream() >> inSlices; // Read the compressed input picture
        // Check picture was read OK
        if (!du.stream()) {
          cerr << "\rFailed to read compressed frame" << endl;
          continue;
        }
        else clog << endl;
    
        // Reorder quantised coefficients from slice order to transform order
        if (verbose) clog << "Merge slices into full picture" << endl;
        const Picture yuvQCoeffs = merge_blocks(inSlices.yuvSlices);

        if (output==INDICES) {
          //Write quantisation indices as 1 byte unsigned values
          clog << "Writing quantisation indices to output file" << endl;
          outStream << arrayio::wordWidth(1); //1 byte per sample
          outStream << arrayio::unsigned_binary; // unsigned output
          outStream << inSlices.qIndices;
          if (!outStream) {
            cerr << "Failed to write output file \"" << outFileName << "\"" << endl;
            return EXIT_FAILURE; }
          continue; // omit rest of processing for this picture
        }
      
        if (output==QUANTISED) {
          //Write quantised transform output as 4 byte 2's comp values
          clog << "Writing quantised transform coefficients to output file" << endl;
          outStream << pictureio::wordWidth(4); // 4 bytes per sample
          outStream << pictureio::signed_binary; // 2's comp output
          outStream << yuvQCoeffs;
          if (!outStream) {
            cerr << "Failed to write output file \"" << outFileName << "\"" << endl;
            return EXIT_FAILURE; }
          continue; // omit rest of processing for this picture
        }
    
        // Inverse quantise in transform order
        if (verbose) clog << "Inverse quantise" << endl;
        const Picture yuvTransform = inverse_quantise_transform_np(yuvQCoeffs, inSlices.qIndices, qMatrix);

        if (output==TRANSFORM) {
          //Write transform output as 4 byte 2's comp values
          clog << "Writing transform coefficients to output file" << endl;
          outStream << pictureio::wordWidth(4); //4 bytes per sample
          outStream << pictureio::signed_binary;
          outStream << yuvTransform;
          if (!outStream) {
            cerr << "Failed to write output file \"" << outFileName << "\"" << endl;
            return EXIT_FAILURE; }
          continue; // omit rest of processing for this picture
        }

        // Inverse wavelet transform
        if (verbose) clog << "Inverse transform" << endl;
        const Picture outPicture = inverseWaveletTransform(yuvTransform, kernel, waveletDepth, picFormat);
  
        // Copy picture to output frame
        if (verbose) clog << "Copy picture to output frame" << endl;
        if (interlaced) {
          if (pic == 0) {
            const PictureFormat frameFormat(height, width, chromaFormat);
            outFrame.reset(new Frame(frameFormat, interlaced, topFieldFirst));

            outFrame->firstField(outPicture);
            pic++;
            continue;
          }

          outFrame->secondField(outPicture);

          pic = 0;
        }
        else { //progressive
          outFrame->frame(outPicture);
        }

        if (verbose) clog << "Clipping output" << endl;
        {
          const int yMin = -utils::pow(2, lumaDepth-1);
          const int yMax = utils::pow(2, lumaDepth-1)-1;
          const int uvMin = -utils::pow(2, chromaDepth-1);
          const int uvMax = utils::pow(2, chromaDepth-1)-1;
          outFrame->frame(clip(*outFrame, yMin, yMax, uvMin, uvMax));
        }

        if (verbose) clog << "Writing decoded output file" << endl;
        outStream << pictureio::wordWidth(bytes); // Set number of bytes per value in file
        outStream << pictureio::left_justified;
        outStream << pictureio::offset_binary;
        outStream << pictureio::bitDepth(lumaDepth, chromaDepth); // Set luma and chroma bit depths
        outStream << *outFrame;
        if (!outStream) {
          cerr << "Failed to write output file \"" << outFileName << "\"" << endl;
          return EXIT_FAILURE;
        }

        ++frame;
      }
      break;
    default:
      continue;
    }
  } //End frame loop

} // end of try block

// Report error messages from try block
catch (const std::exception& ex) {
    cout << "Error: " << ex.what() << endl;
    return EXIT_FAILURE;
}

  // Program should never reach here!
  return EXIT_FAILURE;
}
