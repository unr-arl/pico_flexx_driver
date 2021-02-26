/**
 * Copyright 2014 University of Bremen, Institute for Artificial Intelligence
 * Author: Thiemo Wiedemeyer <wiedemeyer@cs.uni-bremen.de>
 *
 * This file is part of pico_flexx_driver.
 *
 * pico_flexx_driver is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pico_flexx_driver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pico_flexx_driver.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include <ros/ros.h>
#include <ros/console.h>
#include <nodelet/nodelet.h>

#include <std_msgs/Header.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/PointCloud2.h>

#include <royale.hpp>

#include <dynamic_reconfigure/server.h>
#include <pico_flexx_driver/pico_flexx_driverConfig.h>
#include <eigen3/Eigen/Dense>

#define PF_DEFAULT_NS       "pico_flexx"
#define PF_TF_OPT_FRAME     "_optical_frame"
#define PF_TOPIC_INFO       "/camera_info"
#define PF_TOPIC_IMAGE_MONO8      "/image_mono8"
#define PF_TOPIC_IMAGE_MONO16     "/image_mono16"
#define PF_TOPIC_IMAGE_DEPTH      "/image_depth"
#define PF_TOPIC_IMAGE_NOISE      "/image_noise"

#define PF_TOPIC_CLOUD_POINTS      	"/points"
#define PF_TOPIC_CLOUD_NOISE      	"/noise"
#define PF_TOPIC_CLOUD_OVER_RANGE   "/over_range"
#define PF_TOPIC_CLOUD_UNDER_RANGE  "/under_range"
#define PF_TOPIC_CLOUD_NO_CONF     	"/no_conf"

#define NO_COLOR        "\033[0m"
#define FG_BLACK        "\033[30m"
#define FG_RED          "\033[31m"
#define FG_GREEN        "\033[32m"
#define FG_YELLOW       "\033[33m"
#define FG_BLUE         "\033[34m"
#define FG_MAGENTA      "\033[35m"
#define FG_CYAN         "\033[36m"

#define OUT_FUNCTION(NAME) ([](const std::string &name)\
{ \
	size_t end = name.rfind('(');\
	if(end == std::string::npos) end = name.size();\
	size_t begin = 1 + name.rfind(' ', end);\
	return name.substr(begin, end - begin);\
}(NAME))
#define OUT_AUX(FUNC_COLOR, MSG_COLOR, STREAM, MSG) STREAM(FUNC_COLOR "[" << OUT_FUNCTION(__PRETTY_FUNCTION__) << "] " MSG_COLOR << MSG << NO_COLOR)

#define OUT_DEBUG(msg) OUT_AUX(FG_BLUE, NO_COLOR, ROS_DEBUG_STREAM, msg)
#define OUT_INFO(msg) OUT_AUX(FG_GREEN, NO_COLOR, ROS_INFO_STREAM, msg)
#define OUT_WARN(msg) OUT_AUX(FG_YELLOW, FG_YELLOW, ROS_WARN_STREAM, msg)
#define OUT_ERROR(msg) OUT_AUX(FG_RED, FG_RED, ROS_ERROR_STREAM, msg)

// sensor params
#define H_FOV_RAD 62.0 * M_PI / 180.0
#define V_FOV_RAD 45.0 * M_PI / 180.0
#define H_RES 		224.0
#define V_RES 		171.0

class PicoFlexx : public royale::IDepthDataListener, public royale::IExposureListener2
{
private:
	enum Topics
	{
		CAMERA_INFO = 0,
		IMAGE_MONO_8,
		IMAGE_MONO_16,
		IMAGE_DEPTH,
		IMAGE_NOISE,
		CLOUD_POINTS,
		CLOUD_NOISE,
		CLOUD_OVER,
		CLOUD_UNDER,
		CLOUD_NO_CONF,
	};

	ros::NodeHandle nh, priv_nh;
	sensor_msgs::CameraInfo cameraInfo;
	std::vector<std::vector<ros::Publisher>> publisher;

	std::vector<std::vector<bool>> status;

	boost::recursive_mutex lockServer;
	dynamic_reconfigure::Server<pico_flexx_driver::pico_flexx_driverConfig> server;
	pico_flexx_driver::pico_flexx_driverConfig configMin, configMax, config;
	std::vector<int> cbExposureTime;

	std::unique_ptr<royale::ICameraDevice> cameraDevice;
	std::unique_ptr<royale::DepthData> data;

	std::mutex lockStatus, lockData, lockTiming;
	std::condition_variable cvNewData;
	bool running, newData;
	std::vector<bool> ignoreNewExposure;
	uint64_t frame, processTime, delayReceived;
	int framesPerTiming;
	std::string baseNameTF;
	std::chrono::high_resolution_clock::time_point startTime;
	std::thread threadProcess;

	std::vector<float> frustum_endpoints_x;
	std::vector<float> frustum_endpoints_y;
	std::vector<float> frustum_endpoints_z;

public:
	PicoFlexx(const ros::NodeHandle &nh = ros::NodeHandle(), const ros::NodeHandle &priv_nh = ros::NodeHandle("~"))
		: royale::IDepthDataListener(), royale::IExposureListener2(), nh(nh), priv_nh(priv_nh), server(lockServer, priv_nh)
	{
		cbExposureTime.resize(2);
		running = false;
		newData = false;
		ignoreNewExposure.resize(2);
		frame = 0;
		framesPerTiming = 25;
		processTime = 0;
		delayReceived = 0;
		int num_sensors =  1;
		publisher.resize(num_sensors);
		status.resize(num_sensors);
		for(int i = 0; i < num_sensors; ++i)
		{
			publisher[i].resize(10);
			status[i].resize(10, false);
		}


		server.getConfigDefault(config);
		server.getConfigMin(configMin);
		server.getConfigMax(configMax);
		configMin.exposure_time = 50;
		configMin.exposure_time_stream2 = 50;

		init_frustrum();
	}

	~PicoFlexx()
	{
	}
	void init_frustrum()
	{
		// FOV Radians -> half
		const float h_lim_2 = H_FOV_RAD / 2.0;
    	const float v_lim_2 = V_FOV_RAD / 2.0;
		const float h_res_inc  = H_FOV_RAD / H_RES;
		const float v_res_inc  = V_FOV_RAD / V_RES;
		const float max_depth = config.max_depth;
		
		frustum_endpoints_x.resize(int(H_RES), 0.0);
		frustum_endpoints_y.resize(int(H_RES), 0.0);
		frustum_endpoints_z.resize(int(V_RES), 0.0);


		// Top to Bottom
		for(int row_i = 0; row_i < V_RES; ++row_i)
		{
			float dv = (row_i*v_res_inc) - v_lim_2;
			float x = float(max_depth * sin(dv));
			frustum_endpoints_x[row_i] = x;
		}
		for(int col_i = 0; col_i < H_RES; ++col_i)
		{
			float dh = (col_i*h_res_inc) - h_lim_2;
			float z = float(max_depth * cos(dh));
			float y = float(max_depth * sin(dh));
			frustum_endpoints_z[col_i] = z;
			frustum_endpoints_y[col_i] = y;
		}
	}
	void start()
	{
		if(!initialize())
		{
			return;
		}
		running = true;

		threadProcess = std::thread(&PicoFlexx::process, this);

		OUT_INFO("waiting for clients to connect");
	}

	void stop()
	{
		cameraDevice->stopCapture();
		running = false;

		threadProcess.join();
	}

	void onNewData(const royale::DepthData *data)
	{
		std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();

		lockTiming.lock();
		delayReceived += (now.time_since_epoch() - std::chrono::duration_cast<std::chrono::nanoseconds>(data->timeStamp)).count();
		lockTiming.unlock();

		lockData.lock();
		this->data = std::unique_ptr<royale::DepthData>(new royale::DepthData);
		this->data->version = data->version;
		this->data->timeStamp = data->timeStamp;
		this->data->streamId = data->streamId;
		this->data->width = data->width;
		this->data->height = data->height;
		this->data->exposureTimes = data->exposureTimes;
		this->data->points = data->points;
		newData = true;
		lockData.unlock();
		cvNewData.notify_one();
	}

	void onNewExposure(const uint32_t newExposureTime, const royale::StreamId streamId)
	{
		size_t streamIndex;
		if (!findStreamIndex(streamId, streamIndex))
		{
			return;
		}

		if(ignoreNewExposure[streamIndex])
		{
			ignoreNewExposure[streamIndex] = false;
			cbExposureTime[streamIndex] = (int)newExposureTime;
			return;
		}

		if (streamIndex == 0)
		{
			if(config.exposure_time == (int)newExposureTime)
			{
				return;
			}

			OUT_DEBUG("exposure changed: " FG_YELLOW << newExposureTime);
			config.exposure_time = (int)newExposureTime;
		}
		else if (streamIndex == 1)
		{
			if(config.exposure_time_stream2 == (int)newExposureTime)
			{
				return;
			}

			OUT_DEBUG("exposure changed (stream2): " FG_YELLOW << newExposureTime);
			config.exposure_time_stream2 = (int)newExposureTime;
		}
		server.updateConfig(config);
	}

	void callbackTopicStatus()
	{
		lockStatus.lock();
		bool clientsConnected = false;
		for(size_t i = 0; i < 1; ++i)
		{
			for(size_t j = 0; j < 10; ++j)
			{
				status[i][j] = publisher[i][j].getNumSubscribers() > 0;
				clientsConnected = clientsConnected || status[i][j];
			}
		}
		clientsConnected = true;
		bool isCapturing(false);
		cameraDevice->isCapturing(isCapturing);
		if(clientsConnected && !isCapturing)
		{
			OUT_INFO("client connected. starting device...");

			lockTiming.lock();
			processTime = 0;
			frame = 0;
			delayReceived = 0;
			lockTiming.unlock();
			ignoreNewExposure[0] = config.exposure_mode == 0; // ignore if manual mode
			ignoreNewExposure[1] = config.exposure_mode_stream2 == 0;

			if(cameraDevice->startCapture() != royale::CameraStatus::SUCCESS)
			{
				OUT_ERROR("could not start capture!");
				running = false;
				ros::shutdown();
			}

			royale::Vector<royale::StreamId> streams;
			cameraDevice->getStreams(streams);

			if(config.exposure_mode == 0
				 && streams.size() >= 1
				 && cbExposureTime[0] != config.exposure_time)
			{
				setExposure((uint32_t)config.exposure_time, streams[0]);
			}

			if(config.exposure_mode_stream2 == 0
				 && streams.size() >= 2
				 && cbExposureTime[1] != config.exposure_time_stream2)
			{
				setExposure((uint32_t)config.exposure_time_stream2, streams[1]);
			}
		}
		else if(!clientsConnected && isCapturing)
		{
			OUT_INFO("no clients connected. stopping device...");
			if(cameraDevice->stopCapture() != royale::CameraStatus::SUCCESS)
			{
				OUT_ERROR("could not stop capture!");
				running = false;
				ros::shutdown();
			}
		}
		lockStatus.unlock();
	}

	void callbackConfig(pico_flexx_driver::pico_flexx_driverConfig &config, uint32_t level)
	{
		if(level == 0xFFFFFFFF)
		{
			return;
		}

		if(level & 0x01)
		{
			royale::Vector<royale::String> useCases;
			cameraDevice->getUseCases(useCases);
			OUT_INFO("reconfigured use case: " << FG_CYAN << useCases.at(config.use_case) << NO_COLOR);
			if(!setUseCase((size_t)config.use_case))
			{
				config.use_case = this->config.use_case;
				return;
			}
			this->config.use_case = config.use_case;

			// Need to explicitly set these parameters because of the following sequence of calls:
			// - setUseCase() above causes the driver to change the exposure times
			// - onNewExposure() is triggered, updates this->config
			// - server.updateConfig() correctly sets the new params
			// - execution returns here; without the following two lines, the values on the server
			//   would be reset to the stale values in config on return from this function
			config.exposure_time = this->config.exposure_time;
			config.exposure_time_stream2 = this->config.exposure_time_stream2;
		}

		if(level & 0x02)
		{
			OUT_INFO("reconfigured exposure_mode: " << FG_CYAN << (config.exposure_mode == 1 ? "automatic" : "manual") << NO_COLOR);

			royale::Vector<royale::StreamId> streams;
			cameraDevice->getStreams(streams);

			if(streams.size() < 1 || !setExposureMode(config.exposure_mode == 1, streams[0]))
			{
				config.exposure_mode = this->config.exposure_mode;
				return;
			}
			this->config.exposure_mode = config.exposure_mode;
		}

		if(level & 0x04)
		{
			OUT_INFO("reconfigured exposure_mode_stream2: " << FG_CYAN << (config.exposure_mode_stream2 == 1 ? "automatic" : "manual") << NO_COLOR);

			royale::Vector<royale::StreamId> streams;
			cameraDevice->getStreams(streams);

			if(streams.size() < 2 || !setExposureMode(config.exposure_mode_stream2 == 1, streams[1]))
			{
				config.exposure_mode_stream2 = this->config.exposure_mode_stream2;
				return;
			}
			this->config.exposure_mode_stream2 = config.exposure_mode_stream2;
		}

		if(level & 0x08)
		{
			OUT_INFO("reconfigured exposure_time: " << FG_CYAN << config.exposure_time << NO_COLOR);
			royale::Vector<royale::StreamId> streams;
			cameraDevice->getStreams(streams);
			royale::ExposureMode exposureMode;
			cameraDevice->getExposureMode(exposureMode);
			bool isCapturing(false);
			cameraDevice->isCapturing(isCapturing);
			if(exposureMode == royale::ExposureMode::AUTOMATIC
				 || (isCapturing && (streams.size() < 1 || !setExposure((uint32_t)config.exposure_time, streams[0]))))
			{
				config.exposure_time = this->config.exposure_time;
				return;
			}
			this->config.exposure_time = config.exposure_time;
		}

		if(level & 0x10)
		{
			OUT_INFO("reconfigured exposure_time_stream2: " << FG_CYAN << config.exposure_time_stream2 << NO_COLOR);
			royale::Vector<royale::StreamId> streams;
			cameraDevice->getStreams(streams);
			royale::ExposureMode exposureMode;
			cameraDevice->getExposureMode(exposureMode);
			bool isCapturing(false);
			cameraDevice->isCapturing(isCapturing);
			if(exposureMode == royale::ExposureMode::AUTOMATIC
				 || (isCapturing && (streams.size() < 2 || !setExposure((uint32_t)config.exposure_time_stream2, streams[1]))))
			{
				config.exposure_time_stream2 = this->config.exposure_time_stream2;
				return;
			}
			this->config.exposure_time_stream2 = config.exposure_time_stream2;
		}

		if(level & 0x20)
		{
			OUT_INFO("reconfigured max_noise: " << FG_CYAN << config.max_noise << " meters" << NO_COLOR);
			OUT_INFO("reconfigured max_free_noise: " << FG_CYAN << config.max_free_noise << " meters" << NO_COLOR);
			OUT_INFO("reconfigured max_depth: " << FG_CYAN << config.max_depth << " meters" << NO_COLOR);
			OUT_INFO("reconfigured min_depth: " << FG_CYAN << config.min_depth << " meters" << NO_COLOR);
			lockStatus.lock();
			this->config.max_noise = config.max_noise;
			this->config.max_free_noise = config.max_free_noise;
			this->config.max_depth = config.max_depth;
			this->config.min_depth = config.min_depth;
			lockStatus.unlock();
		}

		if(level & 0x40)
		{
			OUT_INFO("reconfigured range_factor: " << FG_CYAN << config.range_factor << " meters" << NO_COLOR);
			lockStatus.lock();
			this->config.range_factor = config.range_factor;
			lockStatus.unlock();
		}

		if(level & (0x01 | 0x80))
		{
			// setting use case resets filter level, so we have to do it again here
			OUT_INFO("reconfigured filter_level: " << FG_CYAN << config.filter_level << NO_COLOR);
			if (setFilterLevel(config.filter_level))
			{
				this->config.filter_level = config.filter_level;
			} else {
				config.filter_level = this->config.filter_level;
			}
		}

		if(level & (0x01 | 0x02 | 0x04))
		{
			royale::Pair<uint32_t, uint32_t> limits;
			royale::Vector<royale::StreamId> streams;
			cameraDevice->getStreams(streams);

			if (streams.size() >= 1)
			{
				cameraDevice->getExposureLimits(limits, streams[0]);
				configMin.exposure_time = limits.first;
				configMax.exposure_time = limits.second;
			}
			if (streams.size() >= 2)
			{
				cameraDevice->getExposureLimits(limits, streams[1]);
				configMin.exposure_time_stream2 = limits.first;
				configMax.exposure_time_stream2 = limits.second;
			}

			server.setConfigMin(configMin);
			server.setConfigMax(configMax);
		}
	}

private:

	bool initialize()
	{
		if(running)
		{
			OUT_ERROR("driver is already running!");
			return false;
		}

		bool automaticExposure, automaticExposureStream2;
		int32_t useCase, exposureTime, exposureTimeStream2, queueSize;
		std::string sensor, baseName;
		double maxNoise, maxFreeNoise, rangeFactor;
		double min_depth, max_depth;

		priv_nh.param("base_name", baseName, std::string(PF_DEFAULT_NS));
		priv_nh.param("sensor", sensor, std::string(""));
		priv_nh.param("use_case", useCase, 0);
		priv_nh.param("automatic_exposure", automaticExposure, true);
		priv_nh.param("automatic_exposure", automaticExposureStream2, true);
		priv_nh.param("exposure_time", exposureTime, 1000);
		priv_nh.param("exposure_time_stream2", exposureTimeStream2, 1000);
		priv_nh.param("max_noise", maxNoise, 0.7);
		priv_nh.param("max_free_noise", maxFreeNoise, 0.7);
		priv_nh.param("range_factor", rangeFactor, 2.0);
		priv_nh.param("queue_size", queueSize, 2);
		priv_nh.param("base_name_tf", baseNameTF, baseName);
		priv_nh.param("min_depth", min_depth, 0.1);
		priv_nh.param("max_depth", max_depth, 4.5);

		OUT_INFO("parameter:" << std::endl
						 << "                 base_name: " FG_CYAN << baseName << NO_COLOR << std::endl
						 << "                    sensor: " FG_CYAN << (sensor.empty() ? "default" : sensor) << NO_COLOR << std::endl
						 << "                  use_case: " FG_CYAN << useCase << NO_COLOR << std::endl
						 << "        automatic_exposure: " FG_CYAN << (automaticExposure ? "true" : "false") << NO_COLOR << std::endl
						 << "automatic_exposure_stream2: " FG_CYAN << (automaticExposureStream2 ? "true" : "false") << NO_COLOR << std::endl
						 << "             exposure_time: " FG_CYAN << exposureTime << NO_COLOR << std::endl
						 << "     exposure_time_stream2: " FG_CYAN << exposureTimeStream2 << NO_COLOR << std::endl
						 << "                 max_noise: " FG_CYAN << maxNoise << " meters" NO_COLOR << std::endl
						 << "                 max_free_noise: " FG_CYAN << maxFreeNoise << " meters" NO_COLOR << std::endl
						 << "              range_factor: " FG_CYAN << rangeFactor << NO_COLOR << std::endl
						 << "                queue_size: " FG_CYAN << queueSize << NO_COLOR << std::endl
						 << "              base_name_tf: " FG_CYAN << baseNameTF << NO_COLOR
						 << "              	  min_depth: " FG_CYAN << min_depth << NO_COLOR
						 << "                 max_depth: " FG_CYAN << max_depth << NO_COLOR);

		uint32_t major, minor, patch, build;
		royale::getVersion(major, minor, patch, build);
		OUT_INFO("libroyale version: " FG_CYAN << major << '.' << minor << '.' << patch << '.' << build << NO_COLOR);

		royale::LensParameters params;
		if(!selectCamera(sensor)
			 || !setUseCase((size_t)useCase)
			 || !setExposureModeAllStreams(automaticExposure, automaticExposureStream2)
			 || !getCameraSettings(params)
			 || !createCameraInfo(params))
		{
			return false;
		}

		if(cameraDevice->registerExposureListener(this) != royale::CameraStatus::SUCCESS)
		{
			OUT_ERROR("could not register exposure listener!");
			return false;
		}


		if(cameraDevice->registerDataListener(this) != royale::CameraStatus::SUCCESS)
		{
			OUT_ERROR("could not register data listener!");
			return false;
		}

		setTopics(baseName, queueSize);

		royale::Pair<uint32_t, uint32_t> limits;
		cameraDevice->getExposureLimits(limits);
		configMin.exposure_time = limits.first;
		configMax.exposure_time = limits.second;
		server.setConfigMin(configMin);
		server.setConfigMax(configMax);

		royale::Vector<royale::String> useCases;
		cameraDevice->getUseCases(useCases);

		config.use_case = std::max(std::min(useCase, (int)useCases.size() - 1), 0);
		config.exposure_mode = automaticExposure ? 1 : 0;
		config.exposure_mode_stream2 = automaticExposureStream2 ? 1 : 0;
		config.exposure_time = std::max(std::min(exposureTime, configMax.exposure_time), configMin.exposure_time);
		config.exposure_time_stream2 = std::max(std::min(exposureTimeStream2, configMax.exposure_time_stream2), configMin.exposure_time_stream2);
		config.max_noise = std::max(std::min(0.0175, configMax.max_noise), configMin.max_noise);
		config.max_free_noise = std::max(std::min(0.06, configMax.max_free_noise), configMin.max_free_noise);
		config.range_factor = std::max(std::min(rangeFactor, configMax.range_factor), configMin.range_factor);
		config.min_depth = std::max(std::min(min_depth, configMax.min_depth), configMin.min_depth);
		config.max_depth = std::max(std::min(max_depth, configMax.max_depth), configMin.max_depth);

		server.setConfigDefault(config);

		dynamic_reconfigure::Server<pico_flexx_driver::pico_flexx_driverConfig>::CallbackType f;
		f = boost::bind(&PicoFlexx::callbackConfig, this, _1, _2);
		server.setCallback(f);

		return true;
	}

	void setTopics(const std::string &baseName, const int32_t queueSize)
	{
		publisher.resize(1);
		ros::SubscriberStatusCallback cb = boost::bind(&PicoFlexx::callbackTopicStatus, this);

		publisher[0].resize(10);
		publisher[0][CAMERA_INFO] 	= nh.advertise<sensor_msgs::CameraInfo>( baseName + PF_TOPIC_INFO, queueSize, cb, cb);
		publisher[0][IMAGE_MONO_8] 	= nh.advertise<sensor_msgs::Image>(		 baseName + PF_TOPIC_IMAGE_MONO8, queueSize, cb, cb);
		publisher[0][IMAGE_MONO_16] = nh.advertise<sensor_msgs::Image>(		 baseName + PF_TOPIC_IMAGE_MONO16, queueSize, cb, cb);
		publisher[0][IMAGE_DEPTH] 	= nh.advertise<sensor_msgs::Image>(		 baseName + PF_TOPIC_IMAGE_DEPTH, queueSize, cb, cb);
		publisher[0][IMAGE_NOISE] 	= nh.advertise<sensor_msgs::Image>(		 baseName + PF_TOPIC_IMAGE_NOISE, queueSize, cb, cb);
		publisher[0][CLOUD_POINTS] 	= nh.advertise<sensor_msgs::PointCloud2>(baseName + PF_TOPIC_CLOUD_POINTS, queueSize, cb, cb);
		publisher[0][CLOUD_NOISE] 	= nh.advertise<sensor_msgs::PointCloud2>(baseName + PF_TOPIC_CLOUD_NOISE, queueSize, cb, cb);
		publisher[0][CLOUD_OVER] 	= nh.advertise<sensor_msgs::PointCloud2>(baseName + PF_TOPIC_CLOUD_OVER_RANGE, queueSize, cb, cb);
		publisher[0][CLOUD_UNDER] 	= nh.advertise<sensor_msgs::PointCloud2>(baseName + PF_TOPIC_CLOUD_UNDER_RANGE, queueSize, cb, cb);
		publisher[0][CLOUD_NO_CONF] = nh.advertise<sensor_msgs::PointCloud2>(baseName + PF_TOPIC_CLOUD_NO_CONF, queueSize, cb, cb);
	}

	bool selectCamera(const std::string &id)
	{
		royale::String _id = id;
		royale::CameraManager manager;

		royale::Vector<royale::String> camlist = manager.getConnectedCameraList();
		if(camlist.empty())
		{
			OUT_ERROR("no cameras connected!");
			return false;
		}

		OUT_INFO("Detected " << camlist.size() << " camera(s):");

		if(id.empty())
		{
			_id = camlist[0];
		}

		int index = -1;
		for(size_t i = 0; i < camlist.size(); ++i)
		{
			if(_id == camlist[i])
			{
				index = (int)i;
				OUT_INFO("  " << i << ": " FG_CYAN << camlist[i] << FG_YELLOW " (selected)" << NO_COLOR);
			}
			else
			{
				OUT_INFO("  " << i << ": " FG_CYAN << camlist[i] << NO_COLOR);
			}
		}

		if(index < 0)
		{
			OUT_ERROR("camera with id '" << _id << "' not found!");
			return false;
		}
		cameraDevice = manager.createCamera(camlist[index]);

		if(cameraDevice == nullptr)
		{
			OUT_ERROR("cannot create camera device!");
			return false;
		}

		if(cameraDevice->initialize() != royale::CameraStatus::SUCCESS)
		{
			OUT_ERROR("cannot initialize camera device");
			return false;
		}
		return true;
	}

	bool getCameraSettings(royale::LensParameters &params)
	{
		bool ret = true;
		royale::Vector<royale::String> useCases;
		cameraDevice->getUseCases(useCases);
		royale::String useCase;
		cameraDevice->getCurrentUseCase(useCase);
		royale::ExposureMode expMode;
		cameraDevice->getExposureMode(expMode);
		royale::Pair<uint32_t, uint32_t> limits;
		cameraDevice->getExposureLimits(limits);
		royale::Vector<royale::Pair<royale::String,royale::String>> info;
		cameraDevice->getCameraInfo(info);

		royale::String cameraProperty;
		cameraDevice->getCameraName(cameraProperty);
		OUT_INFO("camera name: " FG_CYAN << cameraProperty << NO_COLOR);
		cameraDevice->getId(cameraProperty);
		OUT_INFO("camera id: " FG_CYAN << cameraProperty << NO_COLOR);
		royale::CameraAccessLevel accessLevel;
		cameraDevice->getAccessLevel(accessLevel);
		OUT_INFO("access level: " FG_CYAN "L" << (int)accessLevel+ 1 << NO_COLOR);
		OUT_INFO("exposure mode: " FG_CYAN << (expMode == royale::ExposureMode::AUTOMATIC ? "automatic" : "manual") << NO_COLOR);
		OUT_INFO("exposure limits: " FG_CYAN << limits.first << " / " << limits.second << NO_COLOR);

		OUT_INFO("camera info:");
		if(info.empty())
		{
			OUT_INFO("  no camera info available!");
		}
		else
		{
			for(size_t i = 0; i < info.size(); ++i)
			{
				OUT_INFO("  " << info[i].first << ": " FG_CYAN << info[i].second << NO_COLOR);
			}
		}

		OUT_INFO("use cases:");
		if(useCases.empty())
		{
			OUT_ERROR("  no use cases available!");
			ret = false;
		}
		else
		{
			for(size_t i = 0; i < useCases.size(); ++i)
			{
				OUT_INFO("  " << i << ": " FG_CYAN << useCases[i] << (useCases[i] == useCase ? FG_YELLOW " (selected)" : "") << NO_COLOR);
			}
		}

		if(cameraDevice->getLensParameters(params) == royale::CameraStatus::SUCCESS)
		{
			OUT_INFO("camera intrinsics:");
			uint16_t maxSensorWidth;
			cameraDevice->getMaxSensorWidth(maxSensorWidth);
			OUT_INFO("width: " FG_CYAN << maxSensorWidth << NO_COLOR);

			uint16_t maxSensorHeight;
			cameraDevice->getMaxSensorHeight(maxSensorHeight);
			OUT_INFO("height: " FG_CYAN << maxSensorHeight << NO_COLOR);
			OUT_INFO("fx: " FG_CYAN << params.focalLength.first
							 << NO_COLOR ", fy: " FG_CYAN << params.focalLength.second
							 << NO_COLOR ", cx: " FG_CYAN << params.principalPoint.first
							 << NO_COLOR ", cy: " FG_CYAN << params.principalPoint.second << NO_COLOR);
			if(params.distortionRadial.size() == 3)
			{
				OUT_INFO("k1: " FG_CYAN << params.distortionRadial[0]
								 << NO_COLOR ", k2: " FG_CYAN << params.distortionRadial[1]
								 << NO_COLOR ", p1: " FG_CYAN << params.distortionTangential.first
								 << NO_COLOR ", p2: " FG_CYAN << params.distortionTangential.second
								 << NO_COLOR ", k3: " FG_CYAN << params.distortionRadial[2] << NO_COLOR);
			}
			else
			{
				OUT_ERROR("distortion model unknown!");
				ret = false;
			}
		}
		else
		{
			OUT_ERROR("could not get lens parameter!");
			ret = false;
		}
		return ret;
	}

	bool setUseCase(const size_t idx)
	{
		royale::Vector<royale::String> useCases;
		cameraDevice->getUseCases(useCases);
		royale::String useCase;
		cameraDevice->getCurrentUseCase(useCase);

		if(useCases.empty())
		{
			OUT_ERROR("no use cases available!");
			return false;
		}

		if(idx >= useCases.size())
		{
			OUT_ERROR("use case invalid!");
			return false;
		}

		if(useCases[idx] == useCase)
		{
			OUT_INFO("use case not changed!");
			return true;
		}

		if(cameraDevice->setUseCase(useCases[idx]) != royale::CameraStatus::SUCCESS)
		{
			OUT_ERROR("could not set use case!");
			return false;
		}
		OUT_INFO("use case changed to: " FG_YELLOW << useCases[idx]);

		std::string name = royale::String::toStdString(useCases[idx]);

		if (name == "Low_Noise_Extended")
		{
			lockTiming.lock();
			framesPerTiming = 5;
			lockTiming.unlock();
			return true;
		}
		if (name == "Fast_Acquisition") {
			lockTiming.lock();
			framesPerTiming = 45;
			lockTiming.unlock();
			return true;
		}

		size_t start, end;

		// handle MODE_9_5FPS_2000 etc.
		end = name.find("FPS");
		start = name.rfind('_', end);
		if (start != std::string::npos)
			start += 1;

		if(end == std::string::npos || start == std::string::npos)
		{
			// handle MODE_MIXED_30_5, MODE_MIXED_50_5
			start = name.find("MIXED_");
			if (start != std::string::npos)
				start += 6;
			end = name.find('_', start);
		}

		if(end == std::string::npos || start == std::string::npos)
		{
			OUT_WARN("could not extract frames per second from operation mode.");
			lockTiming.lock();
			framesPerTiming = 100;
			lockTiming.unlock();
			return true;
		}

		std::string fpsString = name.substr(start, end - start);
		if(fpsString.find_first_not_of("0123456789") != std::string::npos)
		{
			OUT_WARN("could not extract frames per second from operation mode.");
			lockTiming.lock();
			framesPerTiming = 100;
			lockTiming.unlock();
			return true;
		}

		lockTiming.lock();
		framesPerTiming = std::stoi(fpsString) * 5;
		lockTiming.unlock();
		return true;
	}

	bool setExposureModeAllStreams(const bool automatic, const bool automaticStream2)
	{
		bool success = true;
		royale::Vector<royale::StreamId> streams;
		cameraDevice->getStreams(streams);
		if (streams.size() >= 1)
			success &= setExposureMode(automatic, streams[0]);
		if (streams.size() >= 2)
			success &= setExposureMode(automaticStream2, streams[1]);
		return success;
	}

	bool setExposureMode(const bool automatic, const royale::StreamId streamId = 0)
	{
		royale::ExposureMode newMode = automatic ? royale::ExposureMode::AUTOMATIC : royale::ExposureMode::MANUAL;

		royale::ExposureMode exposureMode;
		cameraDevice->getExposureMode(exposureMode, streamId);
		if(newMode == exposureMode)
		{
			OUT_INFO("exposure mode not changed!");
			return true;
		}

		if(cameraDevice->setExposureMode(newMode, streamId) != royale::CameraStatus::SUCCESS)
		{
			OUT_ERROR("could not set operation mode!");
			return false;
		}

		OUT_INFO("exposure mode changed to: " FG_YELLOW << (automatic ? "automatic" : "manual"));
		return true;
	}

	bool setExposure(const uint32_t exposure, const royale::StreamId streamId = 0)
	{
		royale::Pair<uint32_t, uint32_t> limits;
		cameraDevice->getExposureLimits(limits, streamId);

		if(exposure < limits.first || exposure > limits.second)
		{
			OUT_ERROR("exposure outside of limits!");
			return false;
		}

		if(cameraDevice->setExposureTime(exposure, streamId) != royale::CameraStatus::SUCCESS)
		{
			OUT_ERROR("could not set exposure time!");
			return false;
		}

		OUT_INFO("exposure time changed to: " FG_YELLOW << exposure);
		return true;
	}

	bool setFilterLevel(const int level, const royale::StreamId streamId = 0)
	{
#if (defined(royale_VERSION_MAJOR) && defined(royale_VERSION_MINOR) \
		 && (royale_VERSION_MAJOR > 3 || (royale_VERSION_MAJOR == 3 && royale_VERSION_MINOR >= 21)))
		const royale::FilterLevel desiredLevel = (royale::FilterLevel) level;
		royale::FilterLevel currentLevel;
		if (cameraDevice->getFilterLevel(currentLevel) != royale::CameraStatus::SUCCESS)
		{
			OUT_ERROR("could not get filter level!");
			return false;
		}
		OUT_INFO("current filter level: " << FG_CYAN << (int) currentLevel << NO_COLOR);
		this->config.filter_level = (int) currentLevel;

		if (desiredLevel == currentLevel)
		{
			OUT_INFO("filter level unchanged");
			return false;
		}
		if (desiredLevel == royale::FilterLevel::Custom)
		{
			OUT_INFO("filter level 'Custom' can only be read, not set");
			return false;
		}
		if (currentLevel == royale::FilterLevel::Custom)
		{
			OUT_INFO("current filter level is 'Custom', will not overwrite");
			return false;
		}
		if (cameraDevice->setFilterLevel(desiredLevel, streamId) != royale::CameraStatus::SUCCESS)
		{
			OUT_ERROR("could not set filter level!");
			return false;
		}
		OUT_INFO("filter level changed to: " << FG_YELLOW << (int) desiredLevel);
		return true;
#else
		OUT_ERROR("setting the filter level requires royale >= 3.21!");
		return false;
#endif
	}

	bool createCameraInfo(const royale::LensParameters &params)
	{
		if(params.distortionRadial.size() != 3)
		{
			OUT_ERROR("distortion model unknown!" << params.distortionRadial.size());
			return false;
		}

		uint16_t maxSensorHeight;
		cameraDevice->getMaxSensorHeight(maxSensorHeight);
		cameraInfo.height = maxSensorHeight;

		uint16_t maxSensorWidth;
		cameraDevice->getMaxSensorWidth(maxSensorWidth);
		cameraInfo.width = maxSensorWidth;

		cameraInfo.K[0] = params.focalLength.first;
		cameraInfo.K[1] = 0;
		cameraInfo.K[2] = params.principalPoint.first;
		cameraInfo.K[3] = 0;
		cameraInfo.K[4] = params.focalLength.second;
		cameraInfo.K[5] = params.principalPoint.second;
		cameraInfo.K[6] = 0;
		cameraInfo.K[7] = 0;
		cameraInfo.K[8] = 1;

		cameraInfo.R[0] = 1;
		cameraInfo.R[1] = 0;
		cameraInfo.R[2] = 0;
		cameraInfo.R[3] = 0;
		cameraInfo.R[4] = 1;
		cameraInfo.R[5] = 0;
		cameraInfo.R[6] = 0;
		cameraInfo.R[7] = 0;
		cameraInfo.R[8] = 1;

		cameraInfo.P[0] = params.focalLength.first;
		cameraInfo.P[1] = 0;
		cameraInfo.P[2] = params.principalPoint.first;
		cameraInfo.P[3] = 0;
		cameraInfo.P[4] = 0;
		cameraInfo.P[5] = params.focalLength.second;
		cameraInfo.P[6] = params.principalPoint.second;
		cameraInfo.P[7] = 0;
		cameraInfo.P[8] = 0;
		cameraInfo.P[9] = 0;
		cameraInfo.P[10] = 1;
		cameraInfo.P[11] = 0;

		cameraInfo.distortion_model = "plumb_bob";
		cameraInfo.D.resize(5);
		cameraInfo.D[0] = params.distortionRadial[0];
		cameraInfo.D[1] = params.distortionRadial[1];
		cameraInfo.D[2] = params.distortionTangential.first;
		cameraInfo.D[3] = params.distortionTangential.second;
		cameraInfo.D[4] = params.distortionRadial[2];

		return true;
	}

	void process()
	{
		std::unique_ptr<royale::DepthData> data;
		sensor_msgs::CameraInfoPtr msgCameraInfo;
		sensor_msgs::ImagePtr msgImageMono8, msgImageMono16, msgImageDepth, msgImageNoise;
		sensor_msgs::PointCloud2Ptr msgCloudPoints, msgCloudNoise, msgCloudOver, msgCloudUnder, msgCloudNoConf;

		msgCameraInfo = sensor_msgs::CameraInfoPtr(new sensor_msgs::CameraInfo);
		msgImageMono8 = sensor_msgs::ImagePtr(new sensor_msgs::Image);
		msgImageMono16 = sensor_msgs::ImagePtr(new sensor_msgs::Image);
		msgImageDepth = sensor_msgs::ImagePtr(new sensor_msgs::Image);
		msgImageNoise = sensor_msgs::ImagePtr(new sensor_msgs::Image);


		msgCloudPoints = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2);
		msgCloudNoise = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2);
		msgCloudOver = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2);
		msgCloudUnder = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2);
		msgCloudNoConf = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2);

		std::chrono::high_resolution_clock::time_point start, end;
		std::unique_lock<std::mutex> lock(lockData);
		while(running && ros::ok())
		{
			if(!cvNewData.wait_for(lock, std::chrono::milliseconds(300), [this] { return this->newData; }))
			{
				continue;
			}

			start = std::chrono::high_resolution_clock::now();
			this->data.swap(data);
			newData = false;
			lock.unlock();

			lockStatus.lock();
			size_t streamIndex;
			if (findStreamIndex(data->streamId, streamIndex))
			{
				extractData(*data, msgCameraInfo, msgImageMono8, msgImageMono16, msgImageDepth, msgImageNoise,
						msgCloudPoints, msgCloudNoise, msgCloudOver,msgCloudUnder, msgCloudNoConf, streamIndex);
				publish(msgCameraInfo, msgImageMono8, msgImageMono16, msgImageDepth, msgImageNoise,
						msgCloudPoints, msgCloudNoise, msgCloudOver,msgCloudUnder, msgCloudNoConf, streamIndex);
			}
			lockStatus.unlock();

			end = std::chrono::high_resolution_clock::now();
			lockTiming.lock();
			processTime += (end - start).count();

			timings();
			lockTiming.unlock();

			lock.lock();
		}
	}

	bool findStreamIndex(const royale::StreamId streamId, size_t &streamIndex)
	{
		royale::Vector<royale::StreamId> streams;
		cameraDevice->getStreams(streams);
		auto it = std::find(streams.begin(), streams.end(), streamId);
		if (it == streams.end())
		{
			OUT_ERROR("invalid stream ID!");
			return false;
		}
		streamIndex = std::distance(streams.begin(), it);
		return true;
	}

	void init_field_msg(const royale::DepthData &data, sensor_msgs::PointCloud2Ptr &msg, std_msgs::Header header)
	{
		msg->header = header;
		msg->height = data.height;
		msg->width = data.width;
		msg->is_bigendian = false;
		msg->is_dense = false;
		msg->point_step = (uint32_t)(4 * sizeof(float) + sizeof(u_int16_t) + sizeof(u_int8_t));
		msg->row_step = (uint32_t)(msg->point_step * data.width);
		msg->fields.resize(6);
		msg->fields[0].name = "x";
		msg->fields[0].offset = 0;
		msg->fields[0].datatype = sensor_msgs::PointField::FLOAT32;
		msg->fields[0].count = 1;
		msg->fields[1].name = "y";
		msg->fields[1].offset = msg->fields[0].offset + (uint32_t)sizeof(float);
		msg->fields[1].datatype = sensor_msgs::PointField::FLOAT32;
		msg->fields[1].count = 1;
		msg->fields[2].name = "z";
		msg->fields[2].offset = msg->fields[1].offset + (uint32_t)sizeof(float);
		msg->fields[2].datatype = sensor_msgs::PointField::FLOAT32;
		msg->fields[2].count = 1;
		msg->fields[3].name = "noise";
		msg->fields[3].offset = msg->fields[2].offset + (uint32_t)sizeof(float);
		msg->fields[3].datatype = sensor_msgs::PointField::FLOAT32;
		msg->fields[3].count = 1;
		msg->fields[4].name = "intensity";
		msg->fields[4].offset = msg->fields[3].offset + (uint32_t)sizeof(float);
		msg->fields[4].datatype = sensor_msgs::PointField::UINT16;
		msg->fields[4].count = 1;
		msg->fields[5].name = "gray";
		msg->fields[5].offset = msg->fields[4].offset + (uint32_t)sizeof(uint16_t);
		msg->fields[5].datatype = sensor_msgs::PointField::UINT8;
		msg->fields[5].count = 1;
		msg->data.resize(msg->point_step * data.points.size());
	}
	
	
	void extractData(const royale::DepthData &data, sensor_msgs::CameraInfoPtr &msgCameraInfo, 
									sensor_msgs::ImagePtr &msgMono8, sensor_msgs::ImagePtr &msgMono16, 
									sensor_msgs::ImagePtr &msgDepth, sensor_msgs::ImagePtr &msgNoise,
									sensor_msgs::PointCloud2Ptr &msgCloudPoints,sensor_msgs::PointCloud2Ptr &msgCloudNoise,
									sensor_msgs::PointCloud2Ptr &msgCloudOver,sensor_msgs::PointCloud2Ptr &msgCloudUnder,
									sensor_msgs::PointCloud2Ptr &msgCloudNoConf,
									size_t streamIndex = 0)
	{
		std_msgs::Header header;
		header.frame_id = baseNameTF + PF_TF_OPT_FRAME;
		header.seq = 0;
		header.stamp.fromNSec(std::chrono::duration_cast<std::chrono::nanoseconds>(data.timeStamp).count());

		if(status[streamIndex][CAMERA_INFO])
		{
			*msgCameraInfo = cameraInfo;
			msgCameraInfo->header = header;
			msgCameraInfo->height = data.height;
			msgCameraInfo->width = data.width;
		}
		if(	!(status[streamIndex][IMAGE_MONO_8] || status[streamIndex][IMAGE_MONO_16] || status[streamIndex][IMAGE_DEPTH] || 
			  status[streamIndex][IMAGE_NOISE] || status[streamIndex][CLOUD_POINTS] || status[streamIndex][CLOUD_NOISE] || 
			  status[streamIndex][CLOUD_OVER] || status[streamIndex][CLOUD_UNDER] || status[streamIndex][CLOUD_NO_CONF]))
		{
			return;
		}
		msgMono16->header = header;
		msgMono16->height = data.height;
		msgMono16->width = data.width;
		msgMono16->is_bigendian = false;
		msgMono16->encoding = sensor_msgs::image_encodings::MONO16;
		msgMono16->step = (uint32_t)(sizeof(uint16_t) * data.width);
		msgMono16->data.resize(sizeof(uint16_t) * data.points.size());

		msgDepth->header = header;
		msgDepth->height = data.height;
		msgDepth->width = data.width;
		msgDepth->is_bigendian = false;
		msgDepth->encoding = sensor_msgs::image_encodings::TYPE_32FC1;
		msgDepth->step = (uint32_t)(sizeof(float) * data.width);
		msgDepth->data.resize(sizeof(float) * data.points.size());

		msgNoise->header = header;
		msgNoise->height = data.height;
		msgNoise->width = data.width;
		msgNoise->is_bigendian = false;
		msgNoise->encoding = sensor_msgs::image_encodings::TYPE_32FC1;
		msgNoise->step = (uint32_t)(sizeof(float) * data.width);
		msgNoise->data.resize(sizeof(float) * data.points.size());

		init_field_msg(data, msgCloudPoints, header);
		init_field_msg(data, msgCloudNoise, header);
		init_field_msg(data, msgCloudOver, header);
		init_field_msg(data, msgCloudUnder, header);
		init_field_msg(data, msgCloudNoConf, header);

		const royale::DepthPoint *it_data 	= &data.points[0];
		float *it_img_depth 						= (float *)&msgDepth->data[0];
		float *it_img_noise 						= (float *)&msgNoise->data[0];
		uint16_t *it_img_mono_16 			= (uint16_t *)&msgMono16->data[0];

		Eigen::Vector2i pixel;
		const Eigen::Vector2i pixel_center(V_RES/2.0,H_RES/2.0);

		const float maxNoise = (float)config.max_noise;
		const float maxFreeNoise = (float)config.max_free_noise;

		for(size_t i = 0; i < data.points.size(); ++i, ++it_data, ++it_img_depth, ++it_img_mono_16, ++it_img_noise)
		{
			float *it_cloud_points_x = (float *)&msgCloudPoints->data[i * msgCloudPoints->point_step];
			float *it_cloud_points_y = it_cloud_points_x + 1;
			float *it_cloud_points_z = it_cloud_points_y + 1;
			float *it_cloud_points_noise = it_cloud_points_z + 1;                    
			uint16_t *it_cloud_points_intensity = (uint16_t *)(it_cloud_points_noise + 1);   

			float *it_cloud_noise_x = (float *)&msgCloudNoise->data[i * msgCloudNoise->point_step];
			float *it_cloud_noise_y = it_cloud_noise_x + 1;
			float *it_cloud_noise_z = it_cloud_noise_y + 1;
			float *it_cloud_noise_noise = it_cloud_noise_z + 1;                    
			uint16_t *it_cloud_noise_intensity = (uint16_t *)(it_cloud_noise_noise + 1); 

			float *it_cloud_over_x = (float *)&msgCloudOver->data[i * msgCloudOver->point_step];
			float *it_cloud_over_y = it_cloud_over_x + 1;
			float *it_cloud_over_z = it_cloud_over_y + 1;
			float *it_cloud_over_noise = it_cloud_over_z + 1;                    
			uint16_t *it_cloud_over_intensity = (uint16_t *)(it_cloud_over_noise + 1);   

			float *it_cloud_under_x = (float *)&msgCloudUnder->data[i * msgCloudUnder->point_step];
			float *it_cloud_under_y = it_cloud_under_x + 1;
			float *it_cloud_under_z = it_cloud_under_y + 1;
			float *it_cloud_under_noise = it_cloud_under_z + 1;                    
			uint16_t *it_cloud_under_intensity = (uint16_t *)(it_cloud_under_noise + 1);   

			float *it_cloud_no_conf_x = (float *)&msgCloudNoConf->data[i * msgCloudNoConf->point_step];
			float *it_cloud_no_conf_y = it_cloud_no_conf_x + 1;
			float *it_cloud_no_conf_z = it_cloud_no_conf_y + 1;
			float *it_cloud_no_conf_noise = it_cloud_no_conf_z + 1;                    
			uint16_t *it_cloud_no_conf_intensity = (uint16_t *)(it_cloud_no_conf_noise + 1);   

			if(it_data->depthConfidence > 0.0 && 
			it_data->z < (float)config.max_depth && it_data->z > (float)config.min_depth && 
			it_data->noise < maxNoise)
			{
				*it_cloud_points_x = it_data->x;
				*it_cloud_points_y = it_data->y;
				*it_cloud_points_z = it_data->z;
				*it_cloud_points_noise = it_data->noise;
				*it_cloud_points_intensity = it_data->grayValue;
				*it_img_depth = it_data->z;
				*it_img_noise = it_data->noise;
			}
			if(it_data->noise >= maxNoise)
			{

				float x = it_data->x;
				float y = it_data->y;
				float z = it_data->z;

				Eigen::Vector3d vect(x,y,z);
				vect /= vect.norm();
				vect *= config.max_depth;


				*it_cloud_noise_x = vect(0);
				*it_cloud_noise_y = vect(1);
				*it_cloud_noise_z = vect(2);

				*it_cloud_noise_noise = it_data->noise;                    
				*it_cloud_noise_intensity = it_data->grayValue; 
			}
			if(it_data->z > (float)config.max_depth && it_data->noise < maxFreeNoise)
			{
				float x = it_data->x;
				float y = it_data->y;
				float z = it_data->z;

				Eigen::Vector3d vect(x,y,z);
				vect /= vect.norm();
				vect *= config.max_depth;

				*it_cloud_over_x = vect(0);
				*it_cloud_over_y = vect(1);
				*it_cloud_over_z = vect(2);
				*it_cloud_over_noise = it_data->noise;                    
				*it_cloud_over_intensity = it_data->grayValue; 
			}
			if(it_data->z < (float)config.min_depth)
			{
				*it_cloud_under_x = it_data->x;
				*it_cloud_under_y = it_data->y;
				*it_cloud_under_z = config.max_depth; //it_data->z;
				*it_cloud_under_noise = it_data->noise;                    
				*it_cloud_under_intensity = it_data->grayValue; 
			}
			if(it_data->depthConfidence == 0.0)
			{
				*it_cloud_no_conf_x = it_data->x;
				*it_cloud_no_conf_y = it_data->y;
				*it_cloud_no_conf_z = config.max_depth; //it_data->z;
				*it_cloud_no_conf_noise = it_data->noise;                    
				*it_cloud_no_conf_intensity = it_data->grayValue; 
			}
			// else if(itI->noise >= maxNoise)
			// {
			// 	// noise_counter++;
			// }
			// else if(itI->depthConfidence == 0.0 || 
			// 		(itI->z >= config.max_depth && itI->noise < maxNoise) 
			// 		|| itI->z == 0.0)
			// {
			// 	pixel(0) = i / data.width; // row
			// 	pixel(1) = i % data.width; // col
			// 	int crop = 50;
			// 	if( (pixel-pixel_center).norm() < crop)
			// 	{
			// 		*it_free_x = frustum_endpoints_y[pixel(1)];
			// 		*it_free_y = frustum_endpoints_x[pixel(0)];
			// 		*it_free_z = frustum_endpoints_z[pixel(1)];
			// 		*it_free_noise = itI->noise;
			// 		*it_free_intensity = itI->grayValue;
			// 	}
			// }
			*it_img_mono_16 = it_data->grayValue;
		}

		computeMono8(msgMono16, msgMono8, msgCloudPoints);
	}
	void computeMono8(const sensor_msgs::ImageConstPtr &msgMono16, sensor_msgs::ImagePtr &msgMono8, sensor_msgs::PointCloud2Ptr &msgCloud) const
	{
		msgMono8->header = msgMono16->header;
		msgMono8->height = msgMono16->height;
		msgMono8->width = msgMono16->width;
		msgMono8->is_bigendian = msgMono16->is_bigendian;
		msgMono8->encoding = sensor_msgs::image_encodings::MONO8;
		msgMono8->step = (uint32_t)(sizeof(uint8_t) * msgMono8->width);
		msgMono8->data.resize(sizeof(uint8_t) * msgMono8->width * msgMono8->height);

		const uint16_t *pMono16 = (const uint16_t *)&msgMono16->data[0];
		uint8_t *pMono8 = (uint8_t *)&msgMono8->data[0];
		const size_t size = msgMono8->width * msgMono8->height;

		uint64_t sum = 0;
		uint64_t count = 0;

		const uint16_t *itI = pMono16;
		for(size_t i = 0; i < size; ++i, ++itI)
		{
			if(*itI)
			{
				sum += *itI;
				++count;
			}
		}
		const double average = (double)sum / (double)count;
		double deviation = 0;

		itI = pMono16;
		for(size_t i = 0; i < size; ++i, ++itI)
		{
			if(*itI)
			{
				const double diff = (double) * itI - average;
				deviation += (diff * diff);
			}
		}
		deviation = sqrt(deviation / ((double)count - 1.0));

		const uint16_t minV = (uint16_t)std::max(average - config.range_factor * deviation, 0.0);
		const uint16_t maxV = (uint16_t)(std::min(average + config.range_factor * deviation, 65535.0) - minV);
		const double maxVF = 255.0 / (double)maxV;
		uint8_t *itO = pMono8;
		uint8_t *itP = ((uint8_t *)&msgCloud->data[0]) + 4 * sizeof(float) + sizeof(u_int16_t);   // "gray" field
		itI = pMono16;
		for(size_t i = 0; i < size; ++i, ++itI, ++itO, itP += msgCloud->point_step)
		{
			uint16_t v = *itI;
			if(v < minV)
			{
				v = 0;
			}
			else
			{
				v = (uint16_t)(v - minV);
			}
			if(v > maxV)
			{
				v = maxV;
			}

			const uint8_t newV = (uint8_t)((double)v * maxVF);
			*itO = newV;
			*itP = newV;
		}
	}

	void publish(sensor_msgs::CameraInfoPtr &msgCameraInfo, 
									sensor_msgs::ImagePtr &msgMono8, sensor_msgs::ImagePtr &msgMono16, 
									sensor_msgs::ImagePtr &msgDepth, sensor_msgs::ImagePtr &msgNoise,
									sensor_msgs::PointCloud2Ptr &msgCloudPoints,sensor_msgs::PointCloud2Ptr &msgCloudNoise,
									sensor_msgs::PointCloud2Ptr &msgCloudOver,sensor_msgs::PointCloud2Ptr &msgCloudUnder,
									sensor_msgs::PointCloud2Ptr &msgCloudNoConf,
									size_t streamIndex = 0) const
	{

		msgCloudNoise->fields = msgCloudPoints->fields;
		msgCloudNoise->header.frame_id = baseNameTF + PF_TF_OPT_FRAME;
		msgCloudOver->fields = msgCloudPoints->fields;
		msgCloudOver->header.frame_id = baseNameTF + PF_TF_OPT_FRAME;
		msgCloudUnder->fields = msgCloudPoints->fields;
		msgCloudUnder->header.frame_id = baseNameTF + PF_TF_OPT_FRAME;
		msgCloudNoConf->fields = msgCloudPoints->fields;
		msgCloudNoConf->header.frame_id = baseNameTF + PF_TF_OPT_FRAME;

		if(status[streamIndex][CAMERA_INFO])
		{
			publisher[streamIndex][CAMERA_INFO].publish(msgCameraInfo);
			msgCameraInfo = sensor_msgs::CameraInfoPtr(new sensor_msgs::CameraInfo);
		}
		if(status[streamIndex][IMAGE_MONO_8])
		{
			publisher[streamIndex][IMAGE_MONO_8].publish(msgMono8);
			msgMono8 = sensor_msgs::ImagePtr(new sensor_msgs::Image);
		}
		if(status[streamIndex][IMAGE_MONO_16])
		{
			publisher[streamIndex][IMAGE_MONO_16].publish(msgMono16);
			msgMono16 = sensor_msgs::ImagePtr(new sensor_msgs::Image);
		}
		if(status[streamIndex][IMAGE_DEPTH])
		{
			publisher[streamIndex][IMAGE_DEPTH].publish(msgDepth);
			msgDepth = sensor_msgs::ImagePtr(new sensor_msgs::Image);
		}
		if(status[streamIndex][CLOUD_POINTS])
		{
			publisher[streamIndex][CLOUD_POINTS].publish(msgCloudPoints);
			msgCloudPoints = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2);
		}
		if(status[streamIndex][CLOUD_NOISE])
		{
			publisher[streamIndex][CLOUD_NOISE].publish(msgCloudNoise);
			msgCloudNoise = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2);
		}
		if(status[streamIndex][CLOUD_OVER])
		{
			publisher[streamIndex][CLOUD_OVER].publish(msgCloudOver);
			msgCloudOver = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2);
		}
		if(status[streamIndex][CLOUD_UNDER])
		{
			publisher[streamIndex][CLOUD_UNDER].publish(msgCloudUnder);
			msgCloudUnder = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2);
		}
		if(status[streamIndex][CLOUD_NO_CONF])
		{
			publisher[streamIndex][CLOUD_NO_CONF].publish(msgCloudNoConf);
			msgCloudNoConf = sensor_msgs::PointCloud2Ptr(new sensor_msgs::PointCloud2);
		}
	}

	void timings()
	{
		std::chrono::high_resolution_clock::time_point now = std::chrono::high_resolution_clock::now();

		if(!frame)
		{
			startTime = now;
		}
		else if(frame % framesPerTiming == 0)
		{
			// double timePerFrame, framesPerSecond, avgDelay;

			// timePerFrame = ((double)processTime / framesPerTiming) / 1000000.0;
			// framesPerSecond = (double)framesPerTiming / ((double)(now - startTime).count() / 1000000000.0);
			// avgDelay = ((double)delayReceived / (double)framesPerTiming) / 1000000.0;

			processTime = 0;
			startTime = now;
			delayReceived = 0;
			// OUT_INFO("processing: " FG_YELLOW "~" << std::setprecision(4) << timePerFrame << " ms." NO_COLOR
			// 				 " fps: " FG_YELLOW "~" << framesPerSecond << " Hz" NO_COLOR
			// 				 " delay: " FG_YELLOW "~" << avgDelay << " ms." NO_COLOR);
		}
		++frame;
	}
};

class PicoFlexxNodelet : public nodelet::Nodelet
{
private:
	PicoFlexx *picoFlexx;

public:
	PicoFlexxNodelet() : Nodelet(), picoFlexx(NULL)
	{
	}

	~PicoFlexxNodelet()
	{
		if(picoFlexx)
		{
			picoFlexx->stop();
			delete picoFlexx;
		}
	}

	virtual void onInit()
	{
		picoFlexx = new PicoFlexx(getNodeHandle(), getPrivateNodeHandle());
		picoFlexx->start();
	}
};

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(PicoFlexxNodelet, nodelet::Nodelet)

int main(int argc, char **argv)
{
#if EXTENDED_OUTPUT
	ROSCONSOLE_AUTOINIT;
	if(!getenv("ROSCONSOLE_FORMAT"))
	{
		ros::console::g_formatter.tokens_.clear();
		ros::console::g_formatter.init("[${severity}] ${message}");
	}
#endif

	ros::init(argc, argv, PF_DEFAULT_NS);

	PicoFlexx picoFlexx;
	picoFlexx.start();
	ros::spin();
	picoFlexx.stop();
	return 0;
}