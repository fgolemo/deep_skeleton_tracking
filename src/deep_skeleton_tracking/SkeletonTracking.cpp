#include "SkeletonTracking.hpp"

// Gflags in the command line terminal. Check all the options by adding the flag `--help`, e.g. `openpose.bin --help`.
// Note: This command will show you flags for several files. Check only the flags for the file you are checking. E.g. for `openpose.bin`, look for `Flags from examples/openpose/openpose.cpp:`.
// Debugging
DEFINE_int32(logging_level,             3,              "The logging level. Integer in the range [0, 255]. 0 will output any log() message, while 255 will not output any."
                                                        " Current OpenPose library messages are in the range 0-4: 1 for low priority messages and 4 for important ones.");
// OpenPose
DEFINE_string(model_pose,               "COCO",         "Model to be used (e.g. COCO, MPI, MPI_4_layers).");
DEFINE_string(model_folder,             "~/catkin_ws/src/deep_skeleton_tracking/models/",      "Folder where the pose models (COCO and MPI) are located.");
DEFINE_string(net_resolution,           "656x368",      "Multiples of 16.");
DEFINE_string(resolution,               "1280x720",     "The image resolution (display). Use \"-1x-1\" to force the program to use the default images resolution.");
DEFINE_int32(num_gpu_start,             0,              "GPU device start number.");
DEFINE_double(scale_gap,                0.3,            "Scale gap between scales. No effect unless num_scales>1. Initial scale is always 1. If you want to change the initial scale, "
                                                        "you actually want to multiply the `net_resolution` by your desired initial scale.");
DEFINE_int32(num_scales,                1,              "Number of scales to average.");
// OpenPose Rendering
DEFINE_double(alpha_pose,               0.6,            "Blending factor (range 0-1) for the body part rendering. 1 will show it completely, 0 will hide it.");


SkeletonTracking::SkeletonTracking(bool debug)
{
	this->debug = debug;
	if (debug) {
		cv::namedWindow("view");
  		cv::startWindowThread();
	}

	// ------------------------- INITIALIZATION -------------------------
    // Step 1 - Set logging level
        // - 0 will output all the logging messages
        // - 255 will output nothing
    op::check(0 <= FLAGS_logging_level && FLAGS_logging_level <= 255, "Wrong logging_level value.", __LINE__, __FUNCTION__, __FILE__);
    op::ConfigureLog::setPriorityThreshold((op::Priority)FLAGS_logging_level);
    // Step 2 - Read Google flags (user defined configuration)
    cv::Size outputSize;
    cv::Size netInputSize;
    cv::Size netOutputSize;
    op::PoseModel poseModel;
    std::tie(outputSize, netInputSize, netOutputSize, poseModel) = gflagsToOpParameters();
    // Step 3 - Initialize all required classes
    this->cvMatToOpInput = new op::CvMatToOpInput{netInputSize, FLAGS_num_scales, (float)FLAGS_scale_gap};
    this->cvMatToOpOutput = new op::CvMatToOpOutput{outputSize};
    this->poseExtractorCaffe = new op::PoseExtractorCaffe{netInputSize, netOutputSize, outputSize, FLAGS_num_scales, (float)FLAGS_scale_gap, poseModel,
                                              FLAGS_model_folder, FLAGS_num_gpu_start};
    this->poseRenderer = new op::PoseRenderer{netOutputSize, outputSize, poseModel, nullptr, (float)FLAGS_alpha_pose};
    this->opOutputToCvMat = new op::OpOutputToCvMat{outputSize};
    const cv::Size windowedSize = outputSize;
    this->frameDisplayer = new op::FrameDisplayer{windowedSize, "OpenPose Tutorial - Example 1"};
    // Step 4 - Initialize resources on desired thread (in this case single thread, i.e. we init resources here)
    this->poseExtractorCaffe->initializationOnThread();
	this->poseRenderer->initializationOnThread();
}

SkeletonTracking::~SkeletonTracking()
{
	if (this->debug) {
		cv::destroyWindow("view");
	}
	delete cvMatToOpInput;
	delete cvMatToOpOutput;
	delete poseExtractorCaffe;
	delete poseRenderer;
	delete opOutputToCvMat;
	delete frameDisplayer;
	delete poseExtractorCaffe;
	delete poseRenderer;
}

op::PoseModel SkeletonTracking::gflagToPoseModel(const std::string& poseModeString)
{
    op::log("", op::Priority::Low, __LINE__, __FUNCTION__, __FILE__);
    if (poseModeString == "COCO")
        return op::PoseModel::COCO_18;
    else if (poseModeString == "MPI")
        return op::PoseModel::MPI_15;
    else if (poseModeString == "MPI_4_layers")
        return op::PoseModel::MPI_15_4;
    else
    {
        op::error("String does not correspond to any model (COCO, MPI, MPI_4_layers)", __LINE__, __FUNCTION__, __FILE__);
        return op::PoseModel::COCO_18;
    }
}

// Google flags into program variables
std::tuple<cv::Size, cv::Size, cv::Size, op::PoseModel> SkeletonTracking::gflagsToOpParameters()
{
    op::log("", op::Priority::Low, __LINE__, __FUNCTION__, __FILE__);
    // outputSize
    cv::Size outputSize;
    auto nRead = sscanf(FLAGS_resolution.c_str(), "%dx%d", &outputSize.width, &outputSize.height);
    op::checkE(nRead, 2, "Error, resolution format (" +  FLAGS_resolution + ") invalid, should be e.g., 960x540 ", __LINE__, __FUNCTION__, __FILE__);
    // netInputSize
    cv::Size netInputSize;
    nRead = sscanf(FLAGS_net_resolution.c_str(), "%dx%d", &netInputSize.width, &netInputSize.height);
    op::checkE(nRead, 2, "Error, net resolution format (" +  FLAGS_net_resolution + ") invalid, should be e.g., 656x368 (multiples of 16)", __LINE__, __FUNCTION__, __FILE__);
    // netOutputSize
    const auto netOutputSize = netInputSize;
    // poseModel
    const auto poseModel = gflagToPoseModel(FLAGS_model_pose);
    // Check no contradictory flags enabled
    if (FLAGS_alpha_pose < 0. || FLAGS_alpha_pose > 1.)
        op::error("Alpha value for blending must be in the range [0,1].", __LINE__, __FUNCTION__, __FILE__);
    if (FLAGS_scale_gap <= 0. && FLAGS_num_scales > 1)
        op::error("Uncompatible flag configuration: scale_gap must be greater than 0 or num_scales = 1.", __LINE__, __FUNCTION__, __FILE__);
    // Logging and return result
    op::log("", op::Priority::Low, __LINE__, __FUNCTION__, __FILE__);
    return std::make_tuple(outputSize, netInputSize, netOutputSize, poseModel);
}

void SkeletonTracking::imageCallback(const sensor_msgs::ImageConstPtr& msg)
{
	try
	{
		cv::Mat inputImage = cv_bridge::toCvShare(msg, "bgra8")->image;
		// Step 2 - Format input image to OpenPose input and output formats
	    const auto netInputArray = cvMatToOpInput->format(inputImage);
	    double scaleInputToOutput;
	    op::Array<float> outputArray;
	    std::tie(scaleInputToOutput, outputArray) = cvMatToOpOutput->format(inputImage);
	    // Step 3 - Estimate poseKeyPoints
	    poseExtractorCaffe->forwardPass(netInputArray, inputImage.size());
	    const auto poseKeyPoints = poseExtractorCaffe->getPoseKeyPoints();
	    // Step 4 - Render poseKeyPoints
	    poseRenderer->renderPose(outputArray, poseKeyPoints);
	    // Step 5 - OpenPose output format to cv::Mat
		auto outputImage = opOutputToCvMat->formatToCvMat(outputArray);

		if (this->debug) {
			cv::imshow("view", inputImage);
			cv::waitKey(30);	
			frameDisplayer->displayFrame(outputImage, 0);
		}
	}
	catch (cv_bridge::Exception& e)
	{
		ROS_ERROR("Could not convert from '%s' to 'bgra8'.", msg->encoding.c_str());
	}
}
