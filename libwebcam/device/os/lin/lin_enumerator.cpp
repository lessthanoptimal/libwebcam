/*
 * linenumerator.cpp
 *
 *  Created on: Oct 25, 2015
 *      Author: zebul
 */

#ifndef WIN32

#include <dirent.h>
#include <cstring>
#include <fcntl.h>//O_RDONLY
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <algorithm>
#include <unistd.h>//close
#include <array>
#include <memory>
#include <iostream>


#include "lin_enumerator.h"
#include "image/Format.h"

namespace webcam {

template <typename ENUMRABLE_PROPERTY>
class property_enumerator
{
public:
	property_enumerator(ENUMRABLE_PROPERTY & enumerable_property_, int command_)
	:_enumerable_property(enumerable_property_)
	,_command(command_)
	{
		memset(&_enumerable_property, 0, sizeof(_enumerable_property));
	}

	bool next(int fd_)
	{
		int result = ioctl(fd_, _command, &_enumerable_property );
		_enumerable_property.index++;
		return (0 <= result);
	}
private:
	ENUMRABLE_PROPERTY & _enumerable_property;
	int _command;
};

enumerator::enumerator() {

}

enumerator::~enumerator() {

}

static bool is_video_device(const char * name)
{
	std::string::size_type pos = std::string(name).find("video");
	return pos != std::string::npos;
}

// executes a command and returns the stdout as a string
std::string exec(const char* cmd) {
	std::array<char, 128> buffer;
	std::string result;
	std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
	if (!pipe) {
		throw std::runtime_error("popen() failed!");
	}
	while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
		result += buffer.data();
	}
	return result;
}

std::string lookupDeviceSerial( const std::string& device )
{
	std::string cmd = "/bin/udevadm info --name="+device+" 2>/dev/null | grep SERIAL_SHORT | cut -d '=' -f2";
	std::string serial_number = exec(cmd.c_str());
	// this realy should trim the string to remove the \n but that's not trivial in C++
	if( serial_number.size() > 0 && serial_number.at(serial_number.size()-1) == '\n')
		serial_number = serial_number.substr(0,serial_number.size()-1);
	return serial_number;
}

DeviceInfoEnumeration enumerator::enumerate()
{
	DeviceInfoEnumeration device_enumeration;

	DIR * dp = opendir("/dev");
	if (dp == nullptr) {
		return device_enumeration;
	}

	std::vector<std::string> files;
	struct dirent * ep = nullptr;
	while( (ep = readdir(dp)) )
	{
		if (is_video_device(ep->d_name))
		{
			files.push_back(std::string("/dev/") + ep->d_name);
		}
	}
	closedir(dp);

	std::sort(files.begin(), files.end());

	for(const std::string & device_file: files){

		int fd = open(device_file.c_str(), O_RDONLY);
		if(ERROR != fd){
			std::string serial_number = lookupDeviceSerial(device_file);
            std::cout << "device = " << device_file << " serial = " << serial_number << std::endl;
			webcam::DeviceInfo device_info;

			ModelInfo model_info = get_model_info(fd);
			device_info.get_port() = device_file;
			device_info.get_serial() = serial_number;
			device_info.set_model_info(model_info);

			VideoInfoEnumeration video_enumeration = get_video_info_enumeration(fd);
			device_info.set_video_info_enumeration(video_enumeration);

			// query the device to see which controls it supports

//			get_automatic_controls(fd, V4L2_CID_FOCUS_AUTO, device_info.get_focus_info());
			device_info.get_focus_info().automatic = true;
			if( 0 != get_manual_controls(fd, V4L2_CID_FOCUS_ABSOLUTE, device_info.get_focus_info()) ) {
				std::cout << "get_manual_controls focus failed" << std::endl;
			}
			if( 0 != get_automatic_controls(fd, V4L2_CID_EXPOSURE_AUTO, device_info.get_exposure_info())) {
				std::cout << "get_automatic_controls V4L2_CID_EXPOSURE_AUTO failed" << std::endl;
			}
			if( 0 != get_manual_controls(fd, V4L2_CID_EXPOSURE_ABSOLUTE, device_info.get_exposure_info())) {
				std::cout << "get_manual_controls V4L2_CID_EXPOSURE_ABSOLUTE failed" << std::endl;
			}
			if( 0 != get_automatic_controls(fd, V4L2_CID_AUTOGAIN, device_info.get_gain_info()) ) {
				std::cout << "get_automatic_controls V4L2_CID_AUTOGAIN failed" << std::endl;
			}
			if( 0 != get_manual_controls(fd, V4L2_CID_GAIN, device_info.get_gain_info())) {
				std::cout << "get_manual_controls V4L2_CID_GAIN failed" << std::endl;
			}

			device_enumeration.put(device_info);
			close(fd);
		}
	}
	return device_enumeration;

}

ModelInfo enumerator::get_model_info(int fd_)
{
	struct v4l2_capability video_cap;
	int res = ioctl(fd_, VIDIOC_QUERYCAP, &video_cap);
	if(ERROR != res)
	{
		return webcam::ModelInfo(1, (const char*)video_cap.card);
	}
	return webcam::ModelInfo();
}

VideoInfoEnumeration enumerator::get_video_info_enumeration(int fd_)
{
	VideoInfoEnumeration enumeration;

	struct v4l2_fmtdesc vid_fmtdesc;    // Enumerated video formats supported by the device
	property_enumerator<v4l2_fmtdesc> fmt_enum(vid_fmtdesc, VIDIOC_ENUM_FMT);
	vid_fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	while( fmt_enum.next(fd_) )
	{
		int format = lookup_format_four_cc(vid_fmtdesc.pixelformat);
		std::cout << " found format four_cc " << lookup_format(format) << std::endl;
		Resolutions resolutions = get_resolutions(fd_, vid_fmtdesc.pixelformat);
		webcam::VideoInfo video_info(resolutions, format, 0);
		enumeration.put(video_info);
	}
	return enumeration;
}

Resolutions enumerator::get_resolutions(int fd_, unsigned int pixelformat_)
{
	// Technically it can support multiple types. We will ignore that and force it to one type
	Resolutions resolutions;
	struct v4l2_frmsizeenum frmsize;
	struct v4l2_frmivalenum frmival;
	memset(&frmival, 0, sizeof(frmival));

	property_enumerator<v4l2_frmsizeenum> frmsize_enum(frmsize, VIDIOC_ENUM_FRAMESIZES);
	frmsize.pixel_format = pixelformat_;
	frmsize.index = 0;
	while(frmsize_enum.next(fd_)) {

		if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {

			unsigned int width = frmsize.discrete.width;
			unsigned int height = frmsize.discrete.height;

//			property_enumerator<v4l2_frmivalenum> frmival_enum(frmival, VIDIOC_ENUM_FRAMEINTERVALS);
//			frmival.pixel_format = pixelformat_;
//			frmival.width = width;
//			frmival.height = height;
			resolutions._resolutions.push_back(Shape(width, height));

//			while( frmival_enum.next(fd_) ) {
//				//frmival contains frame rating
//			}
		} else if( frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE ) {
			std::cout << "  stepwise resolution. width = " <<
					  frmsize.stepwise.min_width << " to " << frmsize.stepwise.max_width <<
					  " step " << frmsize.stepwise.step_width << std::endl;
			std::cout << "                       height = " <<
					  frmsize.stepwise.min_height << " to " << frmsize.stepwise.max_height <<
					  " step " << frmsize.stepwise.step_height << std::endl;

			resolutions._width_min = frmsize.stepwise.min_width;
			resolutions._width_max = frmsize.stepwise.max_width;
			resolutions._width_step = frmsize.stepwise.step_width;
			resolutions._height_min = frmsize.stepwise.min_height;
			resolutions._height_max = frmsize.stepwise.max_height;
			resolutions._height_step = frmsize.stepwise.step_height;
		}
	}

	return resolutions;
}

int enumerator::get_automatic_controls(int fd, __u32 control, ControlInfo &info)
{
	struct v4l2_queryctrl queryctrl;
	memset(&queryctrl, 0, sizeof(queryctrl));
	int err = 0;

	info.automatic = false;

	queryctrl.id = control;
	if ((err = ioctl (fd, VIDIOC_QUERYCTRL, &queryctrl)) < 0) {
		fprintf (stderr, "ioctl querycontrol ret %d error %d %s \n", err,errno,strerror(errno));
	} else if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
		fprintf (stderr, "control %s disabled \n", (char *) queryctrl.name);
	} else if (queryctrl.flags & V4L2_CTRL_TYPE_BOOLEAN) {
		info.automatic = true;
		return 0;
	} else if (queryctrl.type & V4L2_CTRL_TYPE_INTEGER) {
        info.automatic = true;
		return 0;
	} else {
		fprintf (stderr, "control %s unsupported  \n", (char *) queryctrl.name);
	}
	return err;
}

int enumerator::get_manual_controls(int fd, __u32 control, ControlInfo &info)
{
    struct v4l2_queryctrl queryctrl;
    memset(&queryctrl, 0, sizeof(queryctrl));
    int err = 0;

    info.manual = false;

    queryctrl.id = control;
    if ((err = ioctl (fd, VIDIOC_QUERYCTRL, &queryctrl)) < 0) {
        fprintf (stderr, "ioctl querycontrol error %d \n", errno);
    } else if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
        fprintf (stderr, "control %s disabled \n", (char *) queryctrl.name);
    } else if (queryctrl.flags & V4L2_CTRL_TYPE_BOOLEAN) {
        fprintf (stderr, "control %s returned boolean for control  \n", (char *) queryctrl.name);
//            printf("bool\n");
        return -1;
    } else if (queryctrl.type & V4L2_CTRL_TYPE_INTEGER) {
        info.manual = true;
        info.min = queryctrl.minimum;
        info.max = queryctrl.maximum;
        info.step = queryctrl.step;
        info.default_value = queryctrl.default_value;
//			printf("int %d %d %d %d\n",info.min,info.max,info.default_value,info.step);
        return 0;
    } else {
        fprintf (stderr, "control %s unsupported  \n", (char *) queryctrl.name);
    }
    return err;
}

} /* namespace core */

#endif
