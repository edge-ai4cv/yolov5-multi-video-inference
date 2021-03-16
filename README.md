# Yolov5 multiple video inference

## Features

* Multi-threading yolov5 inference with  multiple video files or IP cameras.
* Display all the detection results within one window when inferencing.
* Save detection results to video files.
* Compatible with x86_64 and Nvidia Jeston platforms. 

## Prerequisites
* Cuda, cuDNN, Tensorrt. You may use [Install the dependencies of tensorrtx](https://github.com/wang-xinyu/tensorrtx/blob/master/tutorials/install.md) as  refernce.
* Opencv with ffmpeg, gstreamer and dnn support. 

## How to build and run
1. Generate yolov5 engine file as described in [wang-xinyu/tensorrt/yolov5](https://github.com/wang-xinyu/tensorrtx/tree/master/yolov5)

2. Build "yolov5-multi-video"

```
git clone 
cd 
mkdir build
cd build
cmake ..
make
```
3. Run "yolov5-multi-video"
```
// for multiple video files. results are saved as AVI format video files
./yolov5-multi-video -f [engine] [video1] [video1] [....]

// for multile IP cameras. results are saved as AVI format video files
./yolov5-multi-video -c [engine] [rtsp://cam1] [rtsp://cam2] [....]

// ------------- below functionalites are reserved from original Git for convenience---------------------
// for batched images in [image folder]. results are saved as JPG image files. 
sudo ./yolov5-multi-video -d [engine] [image folder]  

// for serialize model to engine file. 
./yolov5-multi-video -s [.wts] [.engine] [s/m/l/x or c gd gw]  
```
4. To interrup program, press "Esc" and  you can then access the saved video files. 

##Acknowledgments
* [wang-xinyu/tensorrt/yolov5](https://github.com/wang-xinyu/tensorrtx/tree/master/yolov5) for yolov5 tensorrt implementation.
* [AlexeyAB/darknet](https://github.com/AlexeyAB/darknet) for data structure to pass objects between threads.