#include <iostream>
#include <chrono>
#include <future>
#include <thread>
#include <exception>
#include <vector>
#include <atomic>

#include <opencv2/opencv.hpp>
#include <opencv2/core/types.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>

#include "cuda_utils.h"
#include "logging.h"
#include "common.hpp"
#include "utils.h"
#include "calibrator.h"
#include "passing_one_obj.hpp"

#define USE_FP16  // set USE_INT8 or USE_FP16 or USE_FP32
#define DEVICE 0  // GPU id
#define NMS_THRESH 0.4
#define CONF_THRESH 0.5
#define BATCH_SIZE 1

#define IMGSHOW_COLS 960
#define IMGSHOW_ROWS 540

// stuff we know about the network and the input/output blobs
static const int INPUT_H = Yolo::INPUT_H;
static const int INPUT_W = Yolo::INPUT_W;
static const int CLASS_NUM = Yolo::CLASS_NUM;
static const int OUTPUT_SIZE = Yolo::MAX_OUTPUT_BBOX_COUNT * sizeof(Yolo::Detection) / sizeof(float) + 1;  // we assume the yololayer outputs no more than MAX_OUTPUT_BBOX_COUNT boxes that conf >= 0.1
const char* INPUT_BLOB_NAME = "data";
const char* OUTPUT_BLOB_NAME = "prob";
static Logger gLogger;

std::vector<passing_one_obj<cv::Mat> *> frame_vec;
std::atomic<bool> exit_flag(false);


static int get_width(int x, float gw, int divisor = 8) {
    //return math.ceil(x / divisor) * divisor
    if (int(x * gw) % divisor == 0) {
        return int(x * gw);
    }
    return (int(x * gw / divisor) + 1) * divisor;
}

static int get_depth(int x, float gd) {
    if (x == 1) {
        return 1;
    } else {
        return round(x * gd) > 1 ? round(x * gd) : 1;
    }
}

ICudaEngine* build_engine(unsigned int maxBatchSize, IBuilder* builder, IBuilderConfig* config, DataType dt, float& gd, float& gw, std::string& wts_name) {
    INetworkDefinition* network = builder->createNetworkV2(0U);

    // Create input tensor of shape {3, INPUT_H, INPUT_W} with name INPUT_BLOB_NAME
    ITensor* data = network->addInput(INPUT_BLOB_NAME, dt, Dims3{ 3, INPUT_H, INPUT_W });
    assert(data);

    std::map<std::string, Weights> weightMap = loadWeights(wts_name);
    Weights emptywts{ DataType::kFLOAT, nullptr, 0 };

    /* ------ yolov5 backbone------ */
    auto focus0 = focus(network, weightMap, *data, 3, get_width(64, gw), 3, "model.0");
    auto conv1 = convBlock(network, weightMap, *focus0->getOutput(0), get_width(128, gw), 3, 2, 1, "model.1");
    auto bottleneck_CSP2 = C3(network, weightMap, *conv1->getOutput(0), get_width(128, gw), get_width(128, gw), get_depth(3, gd), true, 1, 0.5, "model.2");
    auto conv3 = convBlock(network, weightMap, *bottleneck_CSP2->getOutput(0), get_width(256, gw), 3, 2, 1, "model.3");
    auto bottleneck_csp4 = C3(network, weightMap, *conv3->getOutput(0), get_width(256, gw), get_width(256, gw), get_depth(9, gd), true, 1, 0.5, "model.4");
    auto conv5 = convBlock(network, weightMap, *bottleneck_csp4->getOutput(0), get_width(512, gw), 3, 2, 1, "model.5");
    auto bottleneck_csp6 = C3(network, weightMap, *conv5->getOutput(0), get_width(512, gw), get_width(512, gw), get_depth(9, gd), true, 1, 0.5, "model.6");
    auto conv7 = convBlock(network, weightMap, *bottleneck_csp6->getOutput(0), get_width(1024, gw), 3, 2, 1, "model.7");
    auto spp8 = SPP(network, weightMap, *conv7->getOutput(0), get_width(1024, gw), get_width(1024, gw), 5, 9, 13, "model.8");

    /* ------ yolov5 head ------ */
    auto bottleneck_csp9 = C3(network, weightMap, *spp8->getOutput(0), get_width(1024, gw), get_width(1024, gw), get_depth(3, gd), false, 1, 0.5, "model.9");
    auto conv10 = convBlock(network, weightMap, *bottleneck_csp9->getOutput(0), get_width(512, gw), 1, 1, 1, "model.10");

    float* deval = reinterpret_cast<float*>(malloc(sizeof(float) * get_width(512, gw) * 2 * 2));
    for (int i = 0; i < get_width(512, gw) * 2 * 2; i++) {
        deval[i] = 1.0;
    }
    Weights deconvwts11{ DataType::kFLOAT, deval, get_width(512, gw) * 2 * 2 };
    IDeconvolutionLayer* deconv11 = network->addDeconvolutionNd(*conv10->getOutput(0), get_width(512, gw), DimsHW{ 2, 2 }, deconvwts11, emptywts);
    deconv11->setStrideNd(DimsHW{ 2, 2 });
    deconv11->setNbGroups(get_width(512, gw));
    weightMap["deconv11"] = deconvwts11;

    ITensor* inputTensors12[] = { deconv11->getOutput(0), bottleneck_csp6->getOutput(0) };
    auto cat12 = network->addConcatenation(inputTensors12, 2);
    auto bottleneck_csp13 = C3(network, weightMap, *cat12->getOutput(0), get_width(1024, gw), get_width(512, gw), get_depth(3, gd), false, 1, 0.5, "model.13");
    auto conv14 = convBlock(network, weightMap, *bottleneck_csp13->getOutput(0), get_width(256, gw), 1, 1, 1, "model.14");

    Weights deconvwts15{ DataType::kFLOAT, deval, get_width(256, gw) * 2 * 2 };
    IDeconvolutionLayer* deconv15 = network->addDeconvolutionNd(*conv14->getOutput(0), get_width(256, gw), DimsHW{ 2, 2 }, deconvwts15, emptywts);
    deconv15->setStrideNd(DimsHW{ 2, 2 });
    deconv15->setNbGroups(get_width(256, gw));
    ITensor* inputTensors16[] = { deconv15->getOutput(0), bottleneck_csp4->getOutput(0) };
    auto cat16 = network->addConcatenation(inputTensors16, 2);

    auto bottleneck_csp17 = C3(network, weightMap, *cat16->getOutput(0), get_width(512, gw), get_width(256, gw), get_depth(3, gd), false, 1, 0.5, "model.17");

    // yolo layer 0
    IConvolutionLayer* det0 = network->addConvolutionNd(*bottleneck_csp17->getOutput(0), 3 * (Yolo::CLASS_NUM + 5), DimsHW{ 1, 1 }, weightMap["model.24.m.0.weight"], weightMap["model.24.m.0.bias"]);
    auto conv18 = convBlock(network, weightMap, *bottleneck_csp17->getOutput(0), get_width(256, gw), 3, 2, 1, "model.18");
    ITensor* inputTensors19[] = { conv18->getOutput(0), conv14->getOutput(0) };
    auto cat19 = network->addConcatenation(inputTensors19, 2);
    auto bottleneck_csp20 = C3(network, weightMap, *cat19->getOutput(0), get_width(512, gw), get_width(512, gw), get_depth(3, gd), false, 1, 0.5, "model.20");
    //yolo layer 1
    IConvolutionLayer* det1 = network->addConvolutionNd(*bottleneck_csp20->getOutput(0), 3 * (Yolo::CLASS_NUM + 5), DimsHW{ 1, 1 }, weightMap["model.24.m.1.weight"], weightMap["model.24.m.1.bias"]);
    auto conv21 = convBlock(network, weightMap, *bottleneck_csp20->getOutput(0), get_width(512, gw), 3, 2, 1, "model.21");
    ITensor* inputTensors22[] = { conv21->getOutput(0), conv10->getOutput(0) };
    auto cat22 = network->addConcatenation(inputTensors22, 2);
    auto bottleneck_csp23 = C3(network, weightMap, *cat22->getOutput(0), get_width(1024, gw), get_width(1024, gw), get_depth(3, gd), false, 1, 0.5, "model.23");
    IConvolutionLayer* det2 = network->addConvolutionNd(*bottleneck_csp23->getOutput(0), 3 * (Yolo::CLASS_NUM + 5), DimsHW{ 1, 1 }, weightMap["model.24.m.2.weight"], weightMap["model.24.m.2.bias"]);

    auto yolo = addYoLoLayer(network, weightMap, det0, det1, det2);
    yolo->getOutput(0)->setName(OUTPUT_BLOB_NAME);
    network->markOutput(*yolo->getOutput(0));

    // Build engine
    builder->setMaxBatchSize(maxBatchSize);
    config->setMaxWorkspaceSize(16 * (1 << 20));  // 16MB
#if defined(USE_FP16)
    config->setFlag(BuilderFlag::kFP16);
#elif defined(USE_INT8)
    std::cout << "Your platform support int8: " << (builder->platformHasFastInt8() ? "true" : "false") << std::endl;
    assert(builder->platformHasFastInt8());
    config->setFlag(BuilderFlag::kINT8);
    Int8EntropyCalibrator2* calibrator = new Int8EntropyCalibrator2(1, INPUT_W, INPUT_H, "./coco_calib/", "int8calib.table", INPUT_BLOB_NAME);
    config->setInt8Calibrator(calibrator);
#endif

    std::cout << "Building engine, please wait for a while..." << std::endl;
    ICudaEngine* engine = builder->buildEngineWithConfig(*network, *config);
    std::cout << "Build engine successfully!" << std::endl;

    // Don't need the network any more
    network->destroy();

    // Release host memory
    for (auto& mem : weightMap)
    {
        free((void*)(mem.second.values));
    }

    return engine;
}

void APIToModel(unsigned int maxBatchSize, IHostMemory** modelStream, float& gd, float& gw, std::string& wts_name) {
    // Create builder
    IBuilder* builder = createInferBuilder(gLogger);
    IBuilderConfig* config = builder->createBuilderConfig();

    // Create model to populate the network, then set the outputs and create an engine
    ICudaEngine* engine = build_engine(maxBatchSize, builder, config, DataType::kFLOAT, gd, gw, wts_name);
    assert(engine != nullptr);

    // Serialize the engine
    (*modelStream) = engine->serialize();

    // Close everything down
    engine->destroy();
    builder->destroy();
    config->destroy();
}

void doInference(IExecutionContext& context, cudaStream_t& stream, void **buffers, float* input, float* output, int batchSize) {
    // DMA input batch data to device, infer on the batch asynchronously, and DMA output back to host
    CUDA_CHECK(cudaMemcpyAsync(buffers[0], input, batchSize * 3 * INPUT_H * INPUT_W * sizeof(float), cudaMemcpyHostToDevice, stream));
    context.enqueue(batchSize, buffers, stream, nullptr);
    CUDA_CHECK(cudaMemcpyAsync(output, buffers[1], batchSize * OUTPUT_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
    cudaStreamSynchronize(stream);
}

void read_video_src(const std::string& video_src, const int& src_id)
{
    cv::VideoCapture cap(video_src); 
    
    // Check if camera opened successfully
    if(!cap.isOpened()){
        std::cout << "error opening video source." << std::endl;
        return;
    } 

    while (!exit_flag.load()) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty())
            break;
        frame_vec[src_id]->send(frame);
    }
    cap.release();
    return;
}


bool parse_args(int argc, char** argv, std::string& wts, std::string& engine, float& gd, float& gw, std::string& img_dir) {
    if (argc < 4) return false;
    if (std::string(argv[1]) == "-s" && (argc == 5 || argc == 7)) {
        wts = std::string(argv[2]);
        engine = std::string(argv[3]);
        auto net = std::string(argv[4]);
        if (net == "s") {
            gd = 0.33;
            gw = 0.50;
        } else if (net == "m") {
            gd = 0.67;
            gw = 0.75;
        } else if (net == "l") {
            gd = 1.0;
            gw = 1.0;
        } else if (net == "x") {
            gd = 1.33;
            gw = 1.25;
        } else if (net == "c" && argc == 7) {
            gd = atof(argv[5]);
            gw = atof(argv[6]);
        } else {
            return false;
        }
    } 
    else if (std::string(argv[1]) == "-d" && argc == 4) {
        engine = std::string(argv[2]);
        img_dir = std::string(argv[3]);
    } 
    else if (std::string(argv[1]) == "-f") {
        engine = std::string(argv[2]);
    } 
    else if (std::string(argv[1]) == "-c") {
        engine = std::string(argv[2]);
    }
    else {
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    cudaSetDevice(DEVICE);

    std::string wts_name = "";
    std::string engine_name = "";
    float gd = 0.0f, gw = 0.0f;
    std::string img_dir;
    if (!parse_args(argc, argv, wts_name, engine_name, gd, gw, img_dir)) {
        std::cerr << "arguments not right!" << std::endl;
        std::cerr << "./yolov5 -s [.wts] [.engine] [s/m/l/x or c gd gw]  // serialize model to engine file." << std::endl;
        std::cerr << "./yolov5 -d [.engine] [video-file-folder]     // run inference with multiple image files and save results." << std::endl;
        std::cerr << "./yolov5 -f [engine-file] [video-file1] [video-file2] [....]      // run inference with multiple video files and save result to output files." << std::endl;
        std::cerr << "./yolov5 -c [engine-file] [rtsp-cam1] [rtsp-cam2] [...]       // run inference with multiple rtsp Ipcam and save result to output files." << std::endl;
        return -1;
    }

    // create a model using the API directly and serialize it to a stream
    if (std::string(argv[1]) == "-s" && !wts_name.empty()) {
        IHostMemory* modelStream{ nullptr };
        APIToModel(BATCH_SIZE, &modelStream, gd, gw, wts_name);
        assert(modelStream != nullptr);
        std::ofstream p(engine_name, std::ios::binary);
        if (!p) {
            std::cerr << "could not open plan output file" << std::endl;
            return -1;
        }
        p.write(reinterpret_cast<const char*>(modelStream->data()), modelStream->size());
        modelStream->destroy();
        return 0;
    }

    // deserialize the .engine and run inference
    std::ifstream file(engine_name, std::ios::binary);
    if (!file.good()) {
        std::cerr << "read " << engine_name << " error!" << std::endl;
        return -1;
    }
    char *trtModelStream = nullptr;
    size_t size = 0;
    file.seekg(0, file.end);
    size = file.tellg();
    file.seekg(0, file.beg);
    trtModelStream = new char[size];
    assert(trtModelStream);
    file.read(trtModelStream, size);
    file.close();

    // prepare input data ---------------------------
    static float data[BATCH_SIZE * 3 * INPUT_H * INPUT_W];
    //for (int i = 0; i < 3 * INPUT_H * INPUT_W; i++)
    //    data[i] = 1.0;
    static float prob[BATCH_SIZE * OUTPUT_SIZE];
    IRuntime* runtime = createInferRuntime(gLogger);
    assert(runtime != nullptr);
    ICudaEngine* engine = runtime->deserializeCudaEngine(trtModelStream, size);
    assert(engine != nullptr);
    IExecutionContext* context = engine->createExecutionContext();
    assert(context != nullptr);
    delete[] trtModelStream;
    assert(engine->getNbBindings() == 2);
    void* buffers[2];
    // In order to bind the buffers, we need to know the names of the input and output tensors.
    // Note that indices are guaranteed to be less than IEngine::getNbBindings()
    const int inputIndex = engine->getBindingIndex(INPUT_BLOB_NAME);
    const int outputIndex = engine->getBindingIndex(OUTPUT_BLOB_NAME);
    assert(inputIndex == 0);
    assert(outputIndex == 1);
    // Create GPU buffers on device
    CUDA_CHECK(cudaMalloc(&buffers[inputIndex], BATCH_SIZE * 3 * INPUT_H * INPUT_W * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&buffers[outputIndex], BATCH_SIZE * OUTPUT_SIZE * sizeof(float)));
    // Create stream
    cudaStream_t stream;
    CUDA_CHECK(cudaStreamCreate(&stream));

    if (std::string(argv[1]) == "-d") {
        std::vector<std::string> file_names;
        if (read_files_in_dir(img_dir.c_str(), file_names) < 0) {
            std::cerr << "read_files_in_dir failed." << std::endl;
            return -1;
        }
        int fcount = 0;
        for (int f = 0; f < (int)file_names.size(); f++) {
            fcount++;
            if (fcount < BATCH_SIZE && f + 1 != (int)file_names.size()) continue;
            for (int b = 0; b < fcount; b++) {
                cv::Mat img = cv::imread(img_dir + "/" + file_names[f - fcount + 1 + b]);
                if (img.empty()) continue;
                cv::Mat pr_img = preprocess_img(img, INPUT_W, INPUT_H); // letterbox BGR to RGB
                int i = 0;
                for (int row = 0; row < INPUT_H; ++row) {
                    uchar* uc_pixel = pr_img.data + row * pr_img.step;
                    for (int col = 0; col < INPUT_W; ++col) {
                        data[b * 3 * INPUT_H * INPUT_W + i] = (float)uc_pixel[2] / 255.0;
                        data[b * 3 * INPUT_H * INPUT_W + i + INPUT_H * INPUT_W] = (float)uc_pixel[1] / 255.0;
                        data[b * 3 * INPUT_H * INPUT_W + i + 2 * INPUT_H * INPUT_W] = (float)uc_pixel[0] / 255.0;
                        uc_pixel += 3;
                        ++i;
                    }
                }
            }

            // Run inference
            auto start = std::chrono::system_clock::now();
            doInference(*context, stream, buffers, data, prob, BATCH_SIZE);
            auto end = std::chrono::system_clock::now();
            std::cout << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() << "ms" << std::endl;
            std::vector<std::vector<Yolo::Detection>> batch_res(fcount);
            for (int b = 0; b < fcount; b++) {
                auto& res = batch_res[b];
                nms(res, &prob[b * OUTPUT_SIZE], CONF_THRESH, NMS_THRESH);
            }
            for (int b = 0; b < fcount; b++) {
                auto& res = batch_res[b];
                //std::cout << res.size() << std::endl;
                cv::Mat img = cv::imread(img_dir + "/" + file_names[f - fcount + 1 + b]);
                for (size_t j = 0; j < res.size(); j++) {
                    cv::Rect r = get_rect(img, res[j].bbox);
                    cv::rectangle(img, r, cv::Scalar(0x27, 0xC1, 0x36), 2);
                    cv::putText(img, std::to_string((int)res[j].class_id), cv::Point(r.x, r.y - 1), cv::FONT_HERSHEY_PLAIN, 1.2, cv::Scalar(0xFF, 0xFF, 0xFF), 2);
                }
                cv::imwrite("_" + file_names[f - fcount + 1 + b], img);
            }
            fcount = 0;
        }
    }
    else if (std::string(argv[1]) == "-f" || std::string(argv[1]) == "-c") {
        bool sync;
        if (std::string(argv[1]) == "-f")
            sync = true;        // video file
        else 
            sync = false;       // video camera

        std::vector<std::future<void>> future_vec;
        std::vector<cv::VideoWriter> out_file_vec;
        int grid_size = 1;
        for (auto i=0; i <argc-3; i++) 
            if (grid_size * grid_size < argc - 2)
                grid_size ++;
        int subimg_cols = IMGSHOW_COLS/grid_size;
        int subimg_rows = IMGSHOW_ROWS/grid_size;

        
        for (auto i=0; i <argc-3; i++) { 
            frame_vec.push_back(new passing_one_obj<cv::Mat>(sync));
            future_vec.push_back(std::async(std::launch::async, read_video_src, std::string(argv[i+3]), i));

            // save video files
            cv::VideoWriter out;
            std::string fullname = std::string(argv[i+3]);
            size_t lastindex = fullname.find_last_of(".");
            std::string rawname = fullname.substr(0, lastindex); 
            if (std::string(argv[1]) == "-f")
                out.open(rawname + "-out.avi", cv::VideoWriter::fourcc('X', 'V', 'I', 'D'), 25.0, cv::Size(subimg_cols, subimg_rows), true); 
            else
                out.open("rtsp-" + std::to_string(i) + "-out.avi", cv::VideoWriter::fourcc('X', 'V', 'I', 'D'), 25.0, cv::Size(subimg_cols, subimg_rows), true);
            out_file_vec.push_back(out);
        }
        
            
        cv::Mat img_show[BATCH_SIZE];
        while (true) {
            int fcount = 0;
            std::vector <cv::Mat> img_display_vec;
            for (int f = 0; f < (int)future_vec.size(); f++) {
                fcount++;
                if (fcount < BATCH_SIZE && f + 1 != (int)future_vec.size()) continue;
                for (int b = 0; b < fcount; b++) {  
                    cv::Mat img = frame_vec[f]->receive();
                    if (img.empty()) continue;
                    img_show[b] = img.clone();
                    cv::Mat pr_img = preprocess_img(img, INPUT_W, INPUT_H); // letterbox BGR to RGB
                    int i = 0;
                    for (int row = 0; row < INPUT_H; ++row) {
                        uchar* uc_pixel = pr_img.data + row * pr_img.step;
                        for (int col = 0; col < INPUT_W; ++col) {
                            data[b * 3 * INPUT_H * INPUT_W + i] = (float)uc_pixel[2] / 255.0;
                            data[b * 3 * INPUT_H * INPUT_W + i + INPUT_H * INPUT_W] = (float)uc_pixel[1] / 255.0;
                            data[b * 3 * INPUT_H * INPUT_W + i + 2 * INPUT_H * INPUT_W] = (float)uc_pixel[0] / 255.0;
                            uc_pixel += 3;
                            ++i;
                        }
                    }
                }

                // Run inference
                doInference(*context, stream, buffers, data, prob, BATCH_SIZE);
                std::vector<std::vector<Yolo::Detection>> batch_res(fcount);
                for (int b = 0; b < fcount; b++) {
                    auto& res = batch_res[b];
                    nms(res, &prob[b * OUTPUT_SIZE], CONF_THRESH, NMS_THRESH);
                }
                for (int b = 0; b < fcount; b++) {
                    auto& res = batch_res[b];
                    for (size_t j = 0; j < res.size(); j++) {
                        cv::Rect r = get_rect(img_show[b], res[j].bbox);
                        cv::rectangle(img_show[b], r, cv::Scalar(0x27, 0xC1, 0x36), 2);
                        cv::putText(img_show[b], std::to_string((int)res[j].class_id), cv::Point(r.x, r.y - 1), cv::FONT_HERSHEY_PLAIN, 1.2, cv::Scalar(0xFF, 0xFF, 0xFF), 2);
                    }
                    // resize image 
                    cv::resize(img_show[b], img_show[b], cv::Size(subimg_cols, subimg_rows), 0, 0, cv::INTER_AREA);
                    // save to display vector
                    img_display_vec.push_back(img_show[b]);
                }
                fcount = 0;
            }
            // display multiple images in a single window 
            cv::Mat img_dst(540, 960, CV_8UC3, cv::Scalar(0,50,0));
            for (int i = 0; i < (int)img_display_vec.size(); i++)   { 
                img_display_vec[i].copyTo(img_dst(cv::Rect((i%grid_size) * subimg_cols, ((i/grid_size)%grid_size) * subimg_rows, subimg_cols, subimg_rows)));
                // write video file
                out_file_vec[i].write(img_display_vec[i]);
            }
            cv::imshow("Objcet Detection Overlay", img_dst);
            if (cv::waitKey(33) == 27) {
                exit_flag.store(true);
                break;
            }
        }  
        cv::destroyWindow("Objcet Detection Overlay");
        
        for (auto i: out_file_vec) 
            i.release();
        std::cout << "videowriter released..." << std::endl;
        
        // clear frames in buffers
        for (int i = 0; i < (int)future_vec.size(); i++) {
            cv::Mat tmp;
            if (frame_vec[i]->is_object_present())
                tmp = frame_vec[i]->receive();
            future_vec[i].get();
        }
    }
    
    // Release stream and buffers
    cudaStreamDestroy(stream);
    CUDA_CHECK(cudaFree(buffers[inputIndex]));
    CUDA_CHECK(cudaFree(buffers[outputIndex]));
    // Destroy the engine
    context->destroy();
    engine->destroy();
    runtime->destroy();

    return 0;
}
