/*
 * vio.cpp
 *
 *  Created on: Sep 19, 2016
 *      Author: kevinsheridan
 */

#include "vio.h"


/*
 * starts all state vectors at 0.0
 */
VIO::VIO()
{
	this->readROSParameters();

	//tf2_ros::TransformListener tf_listener(tfBuffer); // starts a thread which keeps track of transforms in the system

	//feature tracker pass it its params
	this->feature_tracker.setParams(FEATURE_SIMILARITY_THRESHOLD, MIN_EIGEN_VALUE,
			KILL_BY_DISSIMILARITY, NUM_FEATURES, MIN_EIGEN_VALUE);

	//set up image transport
	image_transport::ImageTransport it(nh);
	this->cameraSub = it.subscribeCamera(this->getCameraTopic(), 1, &VIO::cameraCallback, this);

	//setup imu sub
	this->imuSub = nh.subscribe(this->getIMUTopic(), 100, &VIO::imuCallback, this);

	ekf.setGravityMagnitude(this->GRAVITY_MAG); // set the gravity mag

	this->broadcastWorldToOdomTF();

	//setup pointcloudPublisher
	if(PUBLISH_ACTIVE_FEATURES)
		activePointsPub = nh.advertise<sensor_msgs::PointCloud>("/vio/activefeatures", 100);
}

VIO::~VIO()
{

}

void VIO::cameraCallback(const sensor_msgs::ImageConstPtr& img, const sensor_msgs::CameraInfoConstPtr& cam)
{
	ros::Time start = ros::Time::now();
	cv::Mat temp = cv_bridge::toCvShare(img, "mono8")->image.clone();

	//set the K and D matrices
	this->setK(get3x3FromVector(cam->K));
	this->setD(cv::Mat(cam->D, false));

	//undistort the image using the fisheye model
	//ROS_ASSERT(cam->distortion_model == "fisheye");
	//cv::fisheye::undistortImage(temp, temp, this->K, this->D, this->K);

	// set the current frame
	this->setCurrentFrame(temp, cv_bridge::toCvCopy(img, "mono8")->header.stamp);

	// process the frame correspondences
	this->run();

	//get the run time
	ROS_DEBUG_STREAM_THROTTLE(0.5, (ros::Time::now().toSec() - start.toSec()) * 1000 << " milliseconds runtime");

	this->viewImage(this->getCurrentFrame());
}

void VIO::imuCallback(const sensor_msgs::ImuConstPtr& msg)
{
	//ROS_DEBUG_STREAM_THROTTLE(0.1, "accel: " << msg->linear_acceleration);
	this->ekf.addIMUMessage(*msg);
	//ROS_DEBUG_STREAM("time compare " << ros::Time::now().toNSec() - msg->header.stamp.toNSec());
}

cv::Mat VIO::get3x3FromVector(boost::array<double, 9> vec)
{
	cv::Mat mat = cv::Mat(3, 3, CV_32F);
	for(int i = 0; i < 3; i++)
	{
		mat.at<float>(i, 0) = vec.at(3 * i + 0);
		mat.at<float>(i, 1) = vec.at(3 * i + 1);
		mat.at<float>(i, 2) = vec.at(3 * i + 2);
	}

	ROS_DEBUG_STREAM_ONCE("K = " << mat);
	return mat;
}

/*
 * shows cv::Mat
 */
void VIO::viewImage(cv::Mat img){
	cv::imshow("test", img);
	cv::waitKey(30);
}

/*
 * draws frame with its features
 */
void VIO::viewImage(Frame frame){
	cv::Mat img;
	cv::drawKeypoints(frame.image, frame.getKeyPointVectorFromFeatures(), img, cv::Scalar(0, 0, 255));
	this->viewImage(img);

}

/*
 * sets the current frame and computes important
 * info about it
 * finds corners
 * describes corners
 */
void VIO::setCurrentFrame(cv::Mat img, ros::Time t)
{
	if(currentFrame.isFrameSet())
	{
		//first set the last frame to current frame
		lastFrame = currentFrame;
	}

	currentFrame = Frame(img, t, lastFrame.nextFeatureID); // create a frame with a starting ID of the last frame's next id
}

/*
 * runs:
 * feature detection, ranking, flowing
 * motion estimation
 * feature mapping
 */
void VIO::run()
{
	// if there is a last frame, flow features and estimate motion
	if(lastFrame.isFrameSet())
	{
		if(lastFrame.features.size() > 0)
		{
			feature_tracker.flowFeaturesToNewFrame(lastFrame, currentFrame);
			currentFrame.cleanUpFeaturesByKillRadius(this->KILL_RADIUS);
			//this->checkFeatureConsistency(currentFrame, this->FEATURE_SIMILARITY_THRESHOLD);
		}

		//MOTION ESTIMATION
		this->lastState = this->state;
		//this->state = this->estimateMotion(this->lastState, this->lastFrame, this->currentFrame);

		//UPDATE 3D ACTIVE AND INACTIVE FEATURES
		this->update3DFeatures();
	}

	//check the number of 2d features in the current frame
	//if this is below the required amount refill the feature vector with
	//the best new feature. It must not be redundant.

	//ROS_DEBUG_STREAM("feature count: " << currentFrame.features.size());

	if(currentFrame.features.size() < this->NUM_FEATURES)
	{
		//add n new unique features
		//ROS_DEBUG("low on features getting more");
		currentFrame.getAndAddNewFeatures(this->NUM_FEATURES - currentFrame.features.size(), this->FAST_THRESHOLD, this->KILL_RADIUS, this->MIN_NEW_FEATURE_DISTANCE);
		//currentFrame.describeFeaturesWithBRIEF();
	}

	this->broadcastWorldToOdomTF();

	//ROS_DEBUG_STREAM("imu readings: " << this->imuMessageBuffer.size());
}

/*
 * this function uses the previous state and the current state
 * along with the previous frame and current frame to
 * update each 3d feature and add new 3d features if necessary
 * If 3d feature is not updated it will be either removed or added to the inactive list.
 */
void VIO::update3DFeatures()
{

}

/*
 * gets parameters from ROS param server
 */
void VIO::readROSParameters()
{
	//CAMERA TOPIC
	ROS_WARN_COND(!ros::param::has("~cameraTopic"), "Parameter for 'cameraTopic' has not been set");
	ros::param::param<std::string>("~cameraTopic", cameraTopic, DEFAULT_CAMERA_TOPIC);
	ROS_DEBUG_STREAM("camera topic is: " << cameraTopic);

	//IMU TOPIC
	ROS_WARN_COND(!ros::param::has("~imuTopic"), "Parameter for 'imuTopic' has not been set");
	ros::param::param<std::string>("~imuTopic", imuTopic, DEFAULT_IMU_TOPIC);
	ROS_DEBUG_STREAM("IMU topic is: " << imuTopic);

	ros::param::param<std::string>("~imu_frame_name", imu_frame, DEFAULT_IMU_FRAME_NAME);
	ros::param::param<std::string>("~camera_frame_name", camera_frame, DEFAULT_CAMERA_FRAME_NAME);
	ros::param::param<std::string>("~odom_frame_name", odom_frame, DEFAULT_ODOM_FRAME_NAME);
	ros::param::param<std::string>("~center_of_mass_frame_name", CoM_frame, DEFAULT_COM_FRAME_NAME);
	ros::param::param<std::string>("~world_frame_name", world_frame, DEFAULT_WORLD_FRAME_NAME);
	ekf.imu_frame = imu_frame;
	ekf.camera_frame = camera_frame;
	ekf.odom_frame = odom_frame;
	ekf.CoM_frame = CoM_frame;
	ekf.world_frame = world_frame;

	ros::param::param<int>("~fast_threshold", FAST_THRESHOLD, DEFAULT_FAST_THRESHOLD);

	ros::param::param<float>("~feature_kill_radius", KILL_RADIUS, DEFAULT_2D_KILL_RADIUS);

	ros::param::param<int>("~feature_similarity_threshold", FEATURE_SIMILARITY_THRESHOLD, DEFAULT_FEATURE_SIMILARITY_THRESHOLD);
	ros::param::param<bool>("~kill_by_dissimilarity", KILL_BY_DISSIMILARITY, false);

	ros::param::param<float>("~min_eigen_value", MIN_EIGEN_VALUE, DEFAULT_MIN_EIGEN_VALUE);

	ros::param::param<int>("~num_features", NUM_FEATURES, DEFAULT_NUM_FEATURES);

	ros::param::param<int>("~min_new_feature_distance", MIN_NEW_FEATURE_DISTANCE, DEFAULT_MIN_NEW_FEATURE_DIST);

	ros::param::param<double>("~starting_gravity_mag", GRAVITY_MAG, DEFAULT_GRAVITY_MAGNITUDE);

	ros::param::param<double>("~recalibration_threshold", RECALIBRATION_THRESHOLD, DEFAULT_RECALIBRATION_THRESHOLD);

	ros::param::param<bool>("~publish_active_features", PUBLISH_ACTIVE_FEATURES, DEFAULT_PUBLISH_ACTIVE_FEATURES);

	ros::param::param<std::string>("~active_features_topic", ACTIVE_FEATURES_TOPIC, DEFAULT_ACTIVE_FEATURES_TOPIC);
}

/*
 * broadcasts the world to odom transform
 */
void VIO::broadcastWorldToOdomTF()
{
	//ROS_DEBUG_STREAM("state " << this->state.vector);
	static tf::TransformBroadcaster br;
	tf::Transform transform;
	transform.setOrigin(tf::Vector3(state.x(), state.y(), state.z()));

	//ROS_DEBUG_STREAM(this->pose.pose.orientation.w << " " << this->pose.pose.orientation.x);
	transform.setRotation(state.getTFQuaternion());
	br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), this->world_frame, this->odom_frame));
}

/*
 * broadcasts the odom to tempIMU trans
 */
ros::Time VIO::broadcastOdomToTempIMUTF(double roll, double pitch, double yaw, double x, double y, double z)
{
	static tf::TransformBroadcaster br;
	tf::Transform transform;
	transform.setOrigin(tf::Vector3(x, y, z));
	tf::Quaternion q;
	q.setRPY(roll, pitch, yaw);
	//ROS_DEBUG_STREAM(q.getW() << ", " << q.getX() << ", " << q.getY() << ", " << q.getZ());
	transform.setRotation(q);
	ros::Time sendTime = ros::Time::now();
	br.sendTransform(tf::StampedTransform(transform, sendTime, this->camera_frame, "temp_imu_frame"));
	return sendTime;
}

void VIO::correctOrientation(tf::Quaternion q, double certainty)
{
	//check if quats are nan
	ROS_ASSERT(state.q0() == state.q0());
	ROS_ASSERT(q.getW() == q.getW());
	//Takes orientation and rotates it towards q.
	state.setQuaternion(state.getTFQuaternion().slerp(q, certainty));
}
/*
 * recalibrates the state using average pixel motion
 * uses an Extended Kalman Filter to predict and update the state and its
 * covariance.
 */
VIOState VIO::estimateMotion(VIOState x, Frame lastFrame, Frame currentFrame)
{
	// recalibrate
	static bool consecutiveRecalibration = false;
	double avgFeatureChange = feature_tracker.averageFeatureChange(lastFrame, currentFrame); // get the feature change between f1 and f2

	//recalibrate the state using avg pixel change and track consecutive runs
	if(avgFeatureChange <= this->RECALIBRATION_THRESHOLD)
	{
		this->recalibrateState(avgFeatureChange, this->RECALIBRATION_THRESHOLD, consecutiveRecalibration);
		consecutiveRecalibration = true;
	}
	else
	{
		consecutiveRecalibration = false;
	}

	//run ekf predict step.
	//this will update the state using imu measurements
	//it will also propagate the error throughout the predction step into the states covariance matrix
	//ekf.predict(x, currentFrame.timeImageCreated);

}



/*
 * uses epipolar geometry from two frames to
 * estimate relative motion of the frame;
 */
bool VIO::visualMotionInference(Frame frame1, Frame frame2, tf::Vector3 angleChangePrediction,
		tf::Vector3& rotationInference, tf::Vector3& unitVelocityInference, double& averageMovement)
{
	//first get the feature deltas from the two frames
	std::vector<cv::Point2f> prevPoints, currentPoints;
	feature_tracker.getCorrespondingPointsFromFrames(frame1, frame2, prevPoints, currentPoints);

	//undistort points using fisheye model
	//cv::fisheye::undistortPoints(prevPoints, prevPoints, this->K, this->D);
	//cv::fisheye::undistortPoints(currentPoints, currentPoints, this->K, this->D);

	//get average movement bewteen images
	averageMovement = feature_tracker.averageFeatureChange(prevPoints, currentPoints);

	//ensure that there are enough points to estimate motion with vo
	if(currentPoints.size() < 5)
	{
		return false;
	}

	cv::Mat mask;

	//calculate the essential matrix
	cv::Mat essentialMatrix = cv::findEssentialMat(prevPoints, currentPoints, this->K, cv::RANSAC, 0.999, 1.0, mask);

	//ensure that the essential matrix is the correct size
	if(essentialMatrix.rows != 3 || essentialMatrix.cols != 3)
	{
		return false;
	}

	//recover pose change from essential matrix
	cv::Mat translation;
	cv::Mat rotation;

	//decompose matrix to get possible deltas
	cv::recoverPose(essentialMatrix, prevPoints, currentPoints, this->K, rotation, translation, mask);


	//set the unit velocity inference
	unitVelocityInference.setX(translation.at<double>(0, 0));
	unitVelocityInference.setY(translation.at<double>(1, 0));
	unitVelocityInference.setZ(translation.at<double>(2, 0));

	return true;
}

/*
 * map average pixel change from zero to threshold and then make that a value from 0 to 1
 * So, now you have this value if you multiply it times velocity
 */
void VIO::recalibrateState(double avgPixelChange, double threshold, bool consecutive)
{
	ROS_DEBUG_STREAM("recalibrating with " << avgPixelChange);

	static double lastNormalize = 0;
	static sensor_msgs::Imu lastImu;
	double normalize = avgPixelChange/threshold;
	sensor_msgs::Imu currentImu = ekf.getMostRecentImu();

	ROS_DEBUG_STREAM("normalized pixel change " << normalize);

	state.setVelocity(normalize * state.getVelocity());

	//TODO make a gyro bias measurment vector in the inertial motion estimator and do a weighted average

	gyroNode gNode;
	gNode.gyroBias.setX(currentImu.angular_velocity.x);
	gNode.gyroBias.setY(currentImu.angular_velocity.y);
	gNode.gyroBias.setZ(currentImu.angular_velocity.z);
	gNode.certainty = (1-normalize);
	if(gyroQueue.size() >= DEFAULT_QUEUE_SIZE)
	{
		gyroQueue.pop_back();
		gyroQueue.insert(gyroQueue.begin(), gNode);
	}
	else
	{
		gyroQueue.insert(gyroQueue.begin(), gNode);
	}

	double gyroCertaintySum = 0;
	for(int i=0; i<gyroQueue.size(); ++i)
	{
		gyroCertaintySum += gyroQueue.at(i).certainty;
	}
	std::vector<double> gyroNormlizedCertainty;
	for(int i=0; i<gyroQueue.size(); ++i)
	{
		gyroNormlizedCertainty.push_back(gyroQueue.at(i).certainty / gyroCertaintySum);
	}

	//FINAL WEIGHTED GYROBIASES
	gyroNode gWeightedNode;
	gWeightedNode.gyroBias.setX(0);
	gWeightedNode.gyroBias.setY(0);
	gWeightedNode.gyroBias.setZ(0);
	gWeightedNode.certainty = 0;
	for(int i=0; i<gyroQueue.size(); ++i)
	{
		gWeightedNode.gyroBias.setX(gWeightedNode.gyroBias.getX() + gyroNormlizedCertainty.at(i)*gyroQueue.at(i).gyroBias.getX());
		gWeightedNode.gyroBias.setY(gWeightedNode.gyroBias.getY() + gyroNormlizedCertainty.at(i)*gyroQueue.at(i).gyroBias.getY());
		gWeightedNode.gyroBias.setZ(gWeightedNode.gyroBias.getZ() + gyroNormlizedCertainty.at(i)*gyroQueue.at(i).gyroBias.getZ());
	}

		ekf.gyroBiasX = gWeightedNode.gyroBias.getX();
		ekf.gyroBiasY = gWeightedNode.gyroBias.getY();
		ekf.gyroBiasZ = gWeightedNode.gyroBias.getZ();


	//POTENTIAL BUG
	if(consecutive)
	{
		normalize = (normalize+lastNormalize)/2;

		ROS_DEBUG_STREAM("running consecutive calibration with new normalized " << normalize);

		tf::Vector3 accel(lastImu.linear_acceleration.x*ekf.scaleAccelerometer
				, lastImu.linear_acceleration.y*ekf.scaleAccelerometer
				, lastImu.linear_acceleration.z*ekf.scaleAccelerometer);
		double scale = accel.length();

		//Vector with size DEFAULT_QUEUE_SIZE, elements added at front and dequeued at back
		accelNode aNode;
		aNode.certainty = (1-normalize);
		aNode.accelScale = GRAVITY_MAG/scale;
		if(accelQueue.size() >= DEFAULT_QUEUE_SIZE)
		{
			accelQueue.pop_back();
			accelQueue.insert(accelQueue.begin(), aNode);

		}
		else
		{
			accelQueue.insert(accelQueue.begin(), aNode);
		}
		//Calculating weighted values of gyroBiases and scale
		double accelCertaintySum = 0;
		//		queueNode WeightedValues;
		//		WeightedValues.gyroBias.setX(0);
		//		WeightedValues.gyroBias.setY(0);
		//		WeightedValues.gyroBias.setZ(0);
		//		WeightedValues.certainty = 0;
		//		WeightedValues.scale = 0;
		for(int i=0; i<accelQueue.size(); ++i)
		{
			accelCertaintySum += accelQueue.at(i).certainty;
			//			sum.gyroBias.setX(queue.at(i).gyroBias.getX()+sum.gyroBias.getX());
			//			sum.gyroBias.setY(queue.at(i).gyroBias.getY()+sum.gyroBias.getY());
			//			sum.gyroBias.setZ(queue.at(i).gyroBias.getZ()+sum.gyroBias.getZ());
			//			sum.certainty += queue.at(i).certainty;
			//			sum.scale += queue.at(i).scale;//sum += queue.at(i).certainty;//queue.at(i).scale;
		}

		std::vector<double> accelNormalizedCertainty;
		for(int i=0; i<accelQueue.size(); ++i)
		{
			accelNormalizedCertainty.push_back(accelQueue.at(i).certainty / accelCertaintySum);
			//			Node.certainty = queue.at(i).certainty / sum.certainty;
			//			Node.gyroBias.setX(queue.at(i).gyroBias.getX() / sum.gyroBias.getX());
			//			Node.gyroBias.setY(queue.at(i).gyroBias.getY() / sum.gyroBias.getY());
			//			Node.gyroBias.setZ(queue.at(i).gyroBias.getZ() / sum.gyroBias.getZ());
			//			Node.scale = queue.at(i).scale / sum.scale;
			//
			//			weigthedQueue.push_back(Node);
		}

		//FINAL WEIGHTED ACCELERATION SCALE
		accelNode aWeightedNode;
		aWeightedNode.certainty = 0;
		aWeightedNode.accelScale = 0;

		for(int i=0; i<accelQueue.size(); ++i)
		{
			aWeightedNode.accelScale += accelNormalizedCertainty.at(i)*accelQueue.at(i).accelScale;
			//			WeightedValues.gyroBias.setX(WeightedValues.gyroBias.getX() + normalizedCertainty.at(i)*queue.at(i).gyroBias.getX());
			//			WeightedValues.gyroBias.setY(WeightedValues.gyroBias.getY() + normalizedCertainty.at(i)*queue.at(i).gyroBias.getY());
			//			WeightedValues.gyroBias.setZ(WeightedValues.gyroBias.getZ() + normalizedCertainty.at(i)*queue.at(i).gyroBias.getZ());
			//			WeightedValues.scale += normalizedCertainty.at(i)*queue.at(i).scale;
		}
		//sum *= GRAVITY_MAG/queue.size();
		//TODO create a ten element running wieghted average of the accelerometer scale.
		if(scale != 0)
			ekf.scaleAccelerometer = aWeightedNode.accelScale; // + (normalize)*ekf.scaleAccelerometer;

		tf::Vector3 gravity(0,0,GRAVITY_MAG);

		correctOrientation(ekf.getDifferenceQuaternion(gravity, accel), (1-normalize));

		ROS_DEBUG_STREAM("new acceleration after scaling " << ekf.scaleAccelerometer * accel);
	}

	ROS_DEBUG_STREAM("new accel scale " << ekf.scaleAccelerometer << " new gyro biases "
			<< ekf.gyroBiasX << ", " << ekf.gyroBiasY << ", "
			<< ekf.gyroBiasZ);


	lastImu = currentImu;
	lastNormalize = normalize;
	return;
}




