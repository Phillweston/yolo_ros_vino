#include "yolo_ros_vino/yolo_ros_vino.hpp"

using namespace InferenceEngine;

#define yolo_scale_13 13
#define yolo_scale_26 26
#define yolo_scale_52 52

YoloRosVino::YoloRosVino(ros::NodeHandle nh): 
nodeHandle_(nh),
imageTransport_(nodeHandle_)
{
    ROS_INFO("[YoloRosVino] Node started");
    
    // initialize ROS parameters
    if (!ReadParameters())
        ros::requestShutdown();

    // initialize subscribers and publishers
    imageSubscriber_ = imageTransport_.subscribe(cameraTopicName_, 10, &YoloRosVino::callback, this);
    boundingBoxesPublisher_ = nodeHandle_.advertise<yolo_ros_vino::BoundingBoxes>("bounding_boxes", 1, false);

    //load OpenVINO Plugin for inference engine and required extensions
    InferencePlugin plugin;
    if (neuralComputeStick_){
        plugin = PluginDispatcher({"../lib", ""}).getPluginByDevice("MYRIAD");
    } else {
        plugin = PluginDispatcher({"../lib", ""}).getPluginByDevice("CPU");
        plugin.AddExtension(std::make_shared<Extensions::Cpu::CpuExtensions>());
    }

    // read IR generated by the Model Optimizer (.xml, .bin, .labels files)
    netReader_.ReadNetwork(modelFileName_);
    netReader_.getNetwork().setBatchSize(1);
    netReader_.ReadWeights(binFileName_);
    std::ifstream inputFile(labelFileName_);
    std::copy(std::istream_iterator<std::string>(inputFile), std::istream_iterator<std::string>(), std::back_inserter(this->labels_));

    // configuring input and output
    inputInfo_ = InputsDataMap(netReader_.getNetwork().getInputsInfo());
    InputInfo::Ptr& input = inputInfo_.begin()->second;
    inputName_ = inputInfo_.begin()->first;
    input->setPrecision(Precision::U8);
    input->getInputData()->setLayout(Layout::NCHW);
    outputInfo_ = OutputsDataMap(netReader_.getNetwork().getOutputsInfo());
    for (auto &output : outputInfo_) {
        output.second->setPrecision(Precision::FP32);
        output.second->setLayout(Layout::NCHW);
    }

    // load model to the plugin and create inference requset
    ExecutableNetwork network = plugin.LoadNetwork(netReader_.getNetwork(), {});
    async_infer_request_curr_ = network.CreateInferRequestPtr();
} // end constructor


YoloRosVino::~YoloRosVino(){ }


bool YoloRosVino::ReadParameters(){
    ROS_INFO("[YoloRosVino] Reading ros paramters");

    // load parameters
    nodeHandle_.param("model_thresh", thresh_, (float) 0.3);
    nodeHandle_.param("model_iou_thresh", iouThresh_, (float) 0.4);
    
    nodeHandle_.param("model_xml", modelFileName_, std::string("yolov3_tiny_tags.xml"));
    nodeHandle_.param("model_bin", binFileName_, std::string("yolov3_tiny_tags.bin"));
    nodeHandle_.param("model_labels", labelFileName_, std::string("yolov3_tiny_tags.labels"));
    nodeHandle_.param("neural_compute_stick", neuralComputeStick_, false);
    nodeHandle_.param("camera_topic", cameraTopicName_, std::string("/camera/color/image_raw"));
    nodeHandle_.param("view_result", viewResult_, true);

    // disable viewResult_ if Xserver is not avalible
    if (!XOpenDisplay(NULL) && viewResult_) {
        ROS_INFO("[YoloRosVino] Xserver is not running.");
        viewResult_ = false;
    }
    return true;
}// end ReadParameters


YoloRosVino::DetectionObject::DetectionObject(double x, double y, double h, double w, int class_id, std::string Class, float confidence, float h_scale, float w_scale) {
    this->xmin = static_cast<int>((x - w / 2) * w_scale);
    this->ymin = static_cast<int>((y - h / 2) * h_scale);
    this->xmax = static_cast<int>(this->xmin + w * w_scale);
    this->ymax = static_cast<int>(this->ymin + h * h_scale);
    this->confidence = confidence;
    this->class_id = class_id;
    this->Class = Class;
}// end DetectionObject constructor


yolo_ros_vino::BoundingBox YoloRosVino::DetectionObject::BoundingBox() {
    yolo_ros_vino::BoundingBox boundingBox;
    boundingBox.Class = this->Class;
    boundingBox.probability = this->confidence;
    boundingBox.xmin = this->xmin;
    boundingBox.ymin = this->ymin;
    boundingBox.xmax = this->xmax;
    boundingBox.ymax = this->ymax;
    return boundingBox;
}// end DetectionObject BoundingBox


bool YoloRosVino::DetectionObject::operator<(const DetectionObject &s2) const {
    return this->confidence < s2.confidence;
}// end DetectionObject operator<


int YoloRosVino::EntryIndex(int side, int lcoords, int lclasses, int location, int entry) {
    int n = location / (side * side);
    int loc = location % (side * side);
    return n * side * side * (lcoords + lclasses + 1) + entry * side * side + loc;
}// end EntryIndex


double YoloRosVino::IntersectionOverUnion(const DetectionObject &box_1, const DetectionObject &box_2) {
    double width_of_overlap_area = fmin(box_1.xmax, box_2.xmax) - fmax(box_1.xmin, box_2.xmin);
    double height_of_overlap_area = fmin(box_1.ymax, box_2.ymax) - fmax(box_1.ymin, box_2.ymin);
    double area_of_overlap;
    if (width_of_overlap_area < 0 || height_of_overlap_area < 0)
        area_of_overlap = 0;
    else
        area_of_overlap = width_of_overlap_area * height_of_overlap_area;
    double box_1_area = (box_1.ymax - box_1.ymin)  * (box_1.xmax - box_1.xmin);
    double box_2_area = (box_2.ymax - box_2.ymin)  * (box_2.xmax - box_2.xmin);
    double area_of_union = box_1_area + box_2_area - area_of_overlap;
    return area_of_overlap / area_of_union;
}// end IntersectionOverUnion


void YoloRosVino::ParseYOLOV3Output(const CNNLayerPtr &layer, const Blob::Ptr &blob, const unsigned long resized_im_h, const unsigned long resized_im_w, const unsigned long original_im_h, const unsigned long original_im_w, const float threshold,  std::vector<DetectionObject> &objects) {

    // validating output parameters 
    if (layer->type != "RegionYolo")
        throw std::runtime_error("Invalid output type: " + layer->type + ". RegionYolo expected");
    const int out_blob_h = static_cast<int>(blob->getTensorDesc().getDims()[2]);
    const int out_blob_w = static_cast<int>(blob->getTensorDesc().getDims()[3]);
    if (out_blob_h != out_blob_w)
        throw std::runtime_error("Invalid size of output " + layer->name +
        " It should be in NCHW layout and H should be equal to W. Current H = " + std::to_string(out_blob_h) +
        ", current W = " + std::to_string(out_blob_h));

    // extracting layer parameters 
    auto num = layer->GetParamAsInt("num");
    try { num = layer->GetParamAsInts("mask").size(); } catch (...) {}
    auto coords = layer->GetParamAsInt("coords");
    auto classes = layer->GetParamAsInt("classes");
    std::vector<float> anchors = {10.0, 13.0, 16.0, 30.0, 33.0, 23.0, 30.0, 61.0, 62.0, 45.0, 59.0, 119.0, 116.0, 90.0, 156.0, 198.0, 373.0, 326.0};
    try { anchors = layer->GetParamAsFloats("anchors"); } catch (...) {}
    auto side = out_blob_h;
    int anchor_offset = 0;

    if (anchors.size() == 18) {        // YoloV3
        switch (side) {
            case yolo_scale_13:
                anchor_offset = 2 * 6;
                break;
            case yolo_scale_26:
                anchor_offset = 2 * 3;
                break;
            case yolo_scale_52:
                anchor_offset = 2 * 0;
                break;
            default:
                throw std::runtime_error("Invalid output size");
        }
    } else if (anchors.size() == 12) { // tiny-YoloV3
        switch (side) {
            case yolo_scale_13:
                anchor_offset = 2 * 3;
                break;
            case yolo_scale_26:
                anchor_offset = 2 * 0;
                break;
            default:
                throw std::runtime_error("Invalid output size");
        }
    } else {                           // ???
        switch (side) {
            case yolo_scale_13:
                anchor_offset = 2 * 6;
                break;
            case yolo_scale_26:
                anchor_offset = 2 * 3;
                break;
            case yolo_scale_52:
                anchor_offset = 2 * 0;
                break;
            default:
                throw std::runtime_error("Invalid output size");
        }
    }
    auto side_square = side * side;
    const float *output_blob = blob->buffer().as<PrecisionTrait<Precision::FP32>::value_type *>();

    // parsing YOLO Region output 
    for (int i = 0; i < side_square; ++i) {
        int row = i / side;
        int col = i % side;
        for (int n = 0; n < num; ++n) {
            int obj_index = EntryIndex(side, coords, classes, n * side * side + i, coords);
            int box_index = EntryIndex(side, coords, classes, n * side * side + i, 0);
            float scale = output_blob[obj_index];
            if (scale < threshold)
                continue;
            double x = (col + output_blob[box_index + 0 * side_square]) / side * resized_im_w;
            double y = (row + output_blob[box_index + 1 * side_square]) / side * resized_im_h;
            double height = std::exp(output_blob[box_index + 3 * side_square]) * anchors[anchor_offset + 2 * n + 1];
            double width = std::exp(output_blob[box_index + 2 * side_square]) * anchors[anchor_offset + 2 * n];
            for (int j = 0; j < classes; ++j) {
                int class_index = EntryIndex(side, coords, classes, n * side_square + i, coords + 1 + j);
                float prob = scale * output_blob[class_index];
                if (prob < threshold)
                    continue;
                DetectionObject obj(x, y, height, width, j, this->labels_[j], prob,
                    static_cast<float>(original_im_h) / static_cast<float>(resized_im_h),
                    static_cast<float>(original_im_w) / static_cast<float>(resized_im_w));
                objects.push_back(obj);
            }
        }
    }
}// end ParseYOLOV3Output


void YoloRosVino::callback(const sensor_msgs::ImageConstPtr& current_image)
{
    ROS_INFO_ONCE("[YoloRosVino] Subscribed to camera topic: %s", cameraTopicName_.c_str());

    auto wallclock = std::chrono::high_resolution_clock::now();

    // read image
    cv_bridge::CvImagePtr tempFrame;
    try {
        tempFrame = cv_bridge::toCvCopy(current_image, sensor_msgs::image_encodings::BGR8);
    } catch (cv_bridge::Exception& e) {
        ROS_ERROR("cv_bridge exception: %s", e.what());
        return;
    }
    const size_t width  = (size_t) tempFrame->image.size().width;
    const size_t height = (size_t) tempFrame->image.size().height;

    std_msgs::Header imageHeader = current_image->header;
    cv::Mat frame = tempFrame->image.clone();
    
    // copy data from  image to input blob
    Blob::Ptr frameBlob = async_infer_request_curr_->GetBlob(inputName_);
    matU8ToBlob<uint8_t>(frame, frameBlob);

    // load network
    auto t0 = std::chrono::high_resolution_clock::now();
    async_infer_request_curr_->StartAsync();

    if (OK == async_infer_request_curr_->Wait(IInferRequest::WaitMode::RESULT_READY)) {
        auto t1 = std::chrono::high_resolution_clock::now();
        ms detection = std::chrono::duration_cast<ms>(t1 - t0);

        t0 = std::chrono::high_resolution_clock::now();
        ms wall = std::chrono::duration_cast<ms>(t0 - wallclock);
        wallclock = t0;

        if (viewResult_){
            std::ostringstream out;
            cv::putText(frame, out.str(), cv::Point2f(0, 25), cv::FONT_HERSHEY_TRIPLEX, 0.6, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
            out.str("");
            out << "Wallclock time ";
            out << std::fixed << std::setprecision(2) << wall.count() << " ms (" << 1000.f / wall.count() << " fps)";
            cv::putText(frame, out.str(), cv::Point2f(0, 50), cv::FONT_HERSHEY_TRIPLEX, 0.6, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
            
            out.str("");
            out << "Detection time  : " << std::fixed << std::setprecision(2) << detection.count()
                << " ms ("
                << 1000.f / detection.count() << " fps)";
            cv::putText(frame, out.str(), cv::Point2f(0, 75), cv::FONT_HERSHEY_TRIPLEX, 0.6, cv::Scalar(255, 0, 0), 1, cv::LINE_AA);
        }

        // processing output blobs
        unsigned long resized_im_h = inputInfo_.begin()->second.get()->getDims()[0];
        unsigned long resized_im_w = inputInfo_.begin()->second.get()->getDims()[1];

        // parsing outputs
        std::vector<DetectionObject> objects;
        for (auto &output : outputInfo_) {
            auto output_name = output.first;
            CNNLayerPtr layer = netReader_.getNetwork().getLayerByName(output_name.c_str());
            Blob::Ptr blob = async_infer_request_curr_->GetBlob(output_name);
            ParseYOLOV3Output(layer, blob, resized_im_h, resized_im_w, height, width, thresh_, objects);
        }

        // filtering overlapping boxes
        std::sort(objects.begin(), objects.end());
        for (int i = 0; i < objects.size(); ++i) {
            if (objects[i].confidence == 0)
                continue;
            for (int j = i + 1; j < objects.size(); ++j) {
                if (IntersectionOverUnion(objects[i], objects[j]) >= iouThresh_) {
                    objects[j].confidence = 0;
                }
            }
        }

        // formate results and publish
        if (objects.size()){
            yolo_ros_vino::BoundingBoxes boundingBoxes;
            int i = 1;
            for (auto &object : objects) {
                yolo_ros_vino::BoundingBox boundingBox = object.BoundingBox();
                boundingBoxes.bounding_boxes.push_back(boundingBox);
            }
            boundingBoxes.header.stamp = ros::Time::now();
            boundingBoxes.header.frame_id = "detection";
            boundingBoxes.image_header = imageHeader;
            boundingBoxesPublisher_.publish(boundingBoxes);
        }
        
        // display result using opencv
        for (auto &object : objects) {
            if (object.confidence < thresh_)
                continue;
            auto label = object.class_id;
            float confidence = object.confidence;
            
            ROS_INFO("[YoloRosVino] %s tag (%.2f%%)", this->labels_[label].c_str(), confidence*100);

            if (viewResult_){
                std::ostringstream conf;
                conf << ":" << std::fixed << std::setprecision(3) << confidence;
                cv::putText(frame,
                        (label < this->labels_.size() ? this->labels_[label] : std::string("label #") + std::to_string(label))
                            + conf.str(),
                            cv::Point2f(object.xmin, object.ymin - 5), cv::FONT_HERSHEY_COMPLEX_SMALL, 1, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
                cv::rectangle(frame, cv::Point2f(object.xmin, object.ymin), cv::Point2f(object.xmax, object.ymax), cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
            }
        }
    }

    if (viewResult_){
        cv::imshow("Detection results", frame);
        const int key = cv::waitKey(1);
        if (27 == key)  // Esc
            ros::requestShutdown();
    }

    return;

}// end callback

