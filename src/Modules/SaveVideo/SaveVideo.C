// ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// JeVois Smart Embedded Machine Vision Toolkit - Copyright (C) 2016 by Laurent Itti, the University of Southern
// California (USC), and iLab at USC. See http://iLab.usc.edu and http://jevois.org for information about this project.
//
// This file is part of the JeVois Smart Embedded Machine Vision Toolkit.  This program is free software; you can
// redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software
// Foundation, version 2.  This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
// License for more details.  You should have received a copy of the GNU General Public License along with this program;
// if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//
// Contact information: Laurent Itti - 3641 Watt Way, HNB-07A - Los Angeles, CA 90089-2520 - USA.
// Tel: +1 213 740 3527 - itti@pollux.usc.edu - http://iLab.usc.edu - http://jevois.org
// ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*! \file */

#include <jevois/Core/Module.H>
#include <jevois/Debug/Log.H>
#include <jevois/Image/RawImageOps.H>
#include <jevois/Types/BoundedBuffer.H>

#include <opencv2/core/version.hpp>

#include <opencv2/videoio.hpp> // for cv::VideoCapture
#include <opencv2/imgproc/imgproc.hpp>

#include <future>
#include <linux/videodev2.h> // for v4l2 pixel types
#include <cstdlib> // for std::system()
#include <cstdio> // for snprintf()
#include <fstream>

// icon by Madebyoliver in multimedia at flaticon

static jevois::ParameterCategory const ParamCateg("Video Saving Options");

#define PATHPREFIX "/jevois/data/savevideo/"

//! Parameter \relates SaveVideo
JEVOIS_DECLARE_PARAMETER(filename, std::string, "Name of the video file to write. If path is not absolute, "
                         PATHPREFIX " will be prepended to it. Name should contain a printf-like directive for "
                         "one int argument, which will start at 0 and be incremented on each streamoff command.",
                         "video%06d.avi", ParamCateg);

//! Parameter \relates SaveVideo
JEVOIS_DECLARE_PARAMETER(fourcc, std::string, "FourCC of the codec to use. The OpenCV VideoWriter doc is unclear "
                         "as to which codecs are supported. Presumably, the ffmpeg library is used inside OpenCV. "
                         "Hence any video encoder supported by ffmpeg should work. Tested codecs include: MJPG, "
                         "MP4V, AVC1. Make sure you also pick the right filename extension (e.g., .avi for MJPG, "
                         ".mp4 for MP4V, etc)",
                         "MJPG", boost::regex("^\\w{4}$"), ParamCateg);

//! Parameter \relates SaveVideo
JEVOIS_DECLARE_PARAMETER(fps, double, "Video frames/sec as stored in the file and to be used during playback",
                         30.0, ParamCateg);

//! Save captured camera frames into a video file
/*! Issue the command "start" to start saving video frames, and "stop" to stop saving. Successive start/stop commands
    will increment the file number (%d argument in the 'filename' parameter. Before a file is written, we check whether
    it already exists, and skip over it by incrementing the file number if so.

    This module works with any video resolution and pixel format supported by the camera sensor. Additional video
    mappings are possible beyond the ones listed here.

    When using with no USB output (NONE output format), you should first issue a 'streamon' command to start video
    streaming, then 'start'. The 'streamon' is not necessary when using with a USB video output, the host computer over
    USB triggers video streaming when it starts grabbing frames from the JeVois camera.

    @author Laurent Itti

    @videomapping YUYV 320 240 60.0 YUYV 320 240 60.0 JeVois SaveVideo
    @videomapping YUYV 320 240 30.0 YUYV 320 240 30.0 JeVois SaveVideo
    @videomapping NONE 0 0 0 YUYV 320 240 60.0 JeVois SaveVideo
    @videomapping NONE 0 0 0 YUYV 320 240 30.0 JeVois SaveVideo
    @videomapping NONE 0 0 0 YUYV 176 144 120.0 JeVois SaveVideo
    @email itti\@usc.edu
    @address University of Southern California, HNB-07A, 3641 Watt Way, Los Angeles, CA 90089-2520, USA
    @copyright Copyright (C) 2016 by Laurent Itti, iLab and the University of Southern California
    @mainurl http://jevois.org
    @supporturl http://jevois.org/doc
    @otherurl http://iLab.usc.edu
    @license GPL v3
    @distribution Unrestricted
    @restrictions None
    \ingroup modules */
class SaveVideo : public jevois::Module,
                  public jevois::Parameter<filename, fourcc, fps>
{
  public:
    // ####################################################################################################
    //! Constructor
    // ####################################################################################################
    SaveVideo(std::string const & instance) : jevois::Module(instance), itsBuf(1000), itsSaving(false),
                                              itsFileNum(0), itsRunning(false)
    { }

    // ####################################################################################################
    //! Get started
    // ####################################################################################################
    void postInit() override
    {
      itsRunning.store(true);
      
      // Get our run() thread going, it is in charge of compresing and saving frames:
      itsRunFut = std::async(std::launch::async, &SaveVideo::run, this);
    }

    // ####################################################################################################
    //! Get stopped
    // ####################################################################################################
    void postUninit() override
    {
      // Signal end of run:
      itsRunning.store(false);
      
      // Push an empty frame into our buffer to signal the end of video to our thread:
      itsBuf.push(cv::Mat());

      // Wait for the thread to complete:
      LINFO("Waiting for writer thread to complete, " << itsBuf.filled_size() << " frames to go...");
      try { itsRunFut.get(); } catch (...) { jevois::warnAndIgnoreException(); }
      LINFO("Writer thread completed. Syncing disk...");
      if (std::system("/bin/sync")) LERROR("Error syncing disk -- IGNORED");
      LINFO("Video " << itsFilename << " saved.");
    }
    
    // ####################################################################################################
    //! Virtual destructor for safe inheritance
    // ####################################################################################################
    virtual ~SaveVideo()
    { }
    
    // ####################################################################################################
    //! Processing function, version that also shows a debug video output
    // ####################################################################################################
    void process(jevois::InputFrame && inframe, jevois::OutputFrame && outframe) override
    {
      // Wait for next available camera image:
      jevois::RawImage inimg = inframe.get(true); unsigned int const w = inimg.width, h = inimg.height;
      inimg.require("input", w, h, V4L2_PIX_FMT_YUYV); // accept any image size but require YUYV pixels

      if (itsSaving.load())
      {
        // Convert image to BGR and push to our writer thread:
        if (itsBuf.filled_size() > 1000) LERROR("Image queue too large, video writer cannot keep up - DROPPING FRAME");
        else itsBuf.push(jevois::rawimage::convertToCvBGR(inimg));
      }
      
      // Copy the input image to output:
      jevois::RawImage outimg = outframe.get();
      outimg.require("output", w, h, V4L2_PIX_FMT_YUYV);
      
      jevois::rawimage::paste(inimg, outimg, 0, 0);

      // Let camera know we are done processing the raw YUV input image:
      inframe.done();

      // Show some text messages:
      std::string txt = "SaveVideo: "; if (itsSaving.load()) txt += "RECORDING"; else txt += "not recording";
      jevois::rawimage::writeText(outimg, txt.c_str(), 3, 3, jevois::yuyv::White);
      jevois::rawimage::writeText(outimg, itsFilename.c_str(), 3, h - 13, jevois::yuyv::White);

      // Send output frame over USB:
      outframe.send();
    }

    // ####################################################################################################
    //! Processing function, version with no video output
    // ####################################################################################################
    void process(jevois::InputFrame && inframe) override
    {
      // Wait for next available camera image:
      jevois::RawImage inimg = inframe.get(true);

      if (itsSaving.load())
      {
        // Convert image to BGR and push to our writer thread:
        if (itsBuf.filled_size() > 1000) LERROR("Image queue too large, video writer cannot keep up - DROPPING FRAME");
        else itsBuf.push(jevois::rawimage::convertToCvBGR(inimg));
      }
      
      // Let camera know we are done processing the raw YUV input image:
      inframe.done();
    }

    // ####################################################################################################
    //! Receive a string from a serial port which contains a user command
    // ####################################################################################################
    void parseSerial(std::string const & str, std::shared_ptr<jevois::UserInterface> s) override
    {
      if (str == "start")
      {
        itsSaving.store(true);
        sendSerial("SAVESTART");
      }
      else if (str == "stop")
      {
        itsSaving.store(false);
        sendSerial("SAVESTOP");

        // Push an empty frame into our buffer to signal the end of video to our thread:
        itsBuf.push(cv::Mat());

        // Wait for the thread to empty our image buffer:
        while (itsBuf.filled_size())
        {
          LINFO("Waiting for writer thread to complete, " << itsBuf.filled_size() << " frames to go...");
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        LINFO("Writer thread completed. Syncing disk...");
        if (std::system("/bin/sync")) LERROR("Error syncing disk -- IGNORED");
        LINFO("Video " << itsFilename << " saved.");
      }
      else throw std::runtime_error("Unsupported module command");
    }

    // ####################################################################################################
    //! Human-readable description of this Module's supported custom commands
    // ####################################################################################################
    void supportedCommands(std::ostream & os) override
    {
      os << "start - start saving video" << std::endl;
      os << "stop - stop saving video and increment video file number" << std::endl;
    }

  protected:
    void run() // Runs in a thread
    {
      while (itsRunning.load())
      {
        // Create a VideoWriter here, since it has no close() function, this will ensure it gets destroyed and closes
        // the movie once we stop the recording:
        cv::VideoWriter writer;
        int frame = 0;
      
        while (true)
        {
          // Get next frame from the buffer:
          cv::Mat im = itsBuf.pop();

          // An empty image will be pushed when we are ready to close the video file:
          if (im.empty()) break;
        
          // Start the encoder if it is not yet running:
          if (writer.isOpened() == false)
          {
            // Parse the fourcc, regex in our param definition enforces 4 alphanumeric chars:
            std::string const fcc = fourcc::get();
            int const cvfcc = cv::VideoWriter::fourcc(fcc[0], fcc[1], fcc[2], fcc[3]);
          
            // Add path prefix if given filename is relative:
            std::string fn = filename::get();
            if (fn.empty()) LFATAL("Cannot save to an empty filename");
            if (fn[0] != '/') fn = PATHPREFIX + fn;

            // Create directory just in case it does not exist:
            std::string const cmd = "/bin/mkdir -p " + fn.substr(0, fn.rfind('/'));
            if (std::system(cmd.c_str())) LERROR("Error running [" << cmd << "] -- IGNORED");

            // Fill in the file number; be nice and do not overwrite existing files:
            while (true)
            {
              char tmp[2048];
              std::snprintf(tmp, 2047, fn.c_str(), itsFileNum);
              std::ifstream ifs(tmp);
              if (ifs.is_open() == false) { itsFilename = tmp; break; }
              ++itsFileNum;
            }
            
            // Open the writer:
            if (writer.open(itsFilename, cvfcc, fps::get(), im.size(), true) == false)
              LFATAL("Failed to open video encoder for file [" << itsFilename << ']');

            sendSerial("SAVETO " + itsFilename);
          }

          // Write the frame:
          writer << im;

          // Report what is going on once in a while:
          if ((++frame % 100) == 0) sendSerial("SAVEDNUM " + std::to_string(frame));
        }

        // Our writer runs out of scope and closes the file here.
        ++itsFileNum;
      }
    }
    
    std::future<void> itsRunFut;
    jevois::BoundedBuffer<cv::Mat, jevois::BlockingBehavior::Block, jevois::BlockingBehavior::Block> itsBuf;
    std::atomic<bool> itsSaving;
    int itsFileNum;
    std::atomic<bool> itsRunning;
    std::string itsFilename;
};

// Allow the module to be loaded as a shared object (.so) file:
JEVOIS_REGISTER_MODULE(SaveVideo);
