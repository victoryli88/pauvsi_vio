/*
 * Feature.cpp
 *
 *  Created on: Jan 30, 2017
 *      Author: pauvsi
 */


#include "Feature.h"

Feature::Feature(){
	set = false;
	described = false;
	undistorted = false;
}

Feature::Feature(Frame* _frame, cv::Point2f px, Point* pt, int _id)
{
	set = true;
	described = false;
	undistorted = false;
	frame = _frame;
	//ROS_DEBUG_STREAM("feature's frame Pointer: " << frame);
	feature.pt = px;
	original_pxl = px;
	radius = -1; // the radius is not very important
	quality = 0;

	id = _id;

	description = cv::Mat(cv::Size(0, 0), CV_32F);

	this->undistort(frame->K, frame->D); // assuming that the point is distorted and not normal yet

	if(pt != NULL){
		point = pt;
		point->addObservation(this); // add my self to the point's observations
	}
	else{
		ROS_DEBUG_STREAM("null pointer");
		point = NULL;
	}
}

void Feature::undistort(cv::Mat K, cv::Mat D)
{
	std::vector<cv::Point2f> in;
	in.push_back(this->original_pxl);

	std::vector<cv::Point2f> out;

	cv::fisheye::undistortPoints(in, out, K, D); // the new K is the Identity matrix

	this->undistort_pxl = out.at(0);
	this->undistorted = true;
}


Eigen::Vector3d Feature::getDirectionVector()
{
	ROS_ASSERT(this->undistorted && set);
	ROS_ASSERT(this->point != NULL);
	return Eigen::Vector3d(undistort_pxl.x, undistort_pxl.y, 1.0);
}

Eigen::Vector2d Feature::getUndistortedMeasurement()
{
	ROS_ASSERT(this->undistorted && set);
	return Eigen::Vector2d(undistort_pxl.x, undistort_pxl.y);
}

