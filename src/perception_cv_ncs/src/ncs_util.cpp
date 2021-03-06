//
// Created by pesong on 18-9-5.
//

#include <math.h>

#include "ncs_util.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"



//! movidius preprocessing
bool g_graph_Success;
ncStatus_t retCodeSeg;
ncStatus_t retCodeDet;
struct ncDeviceHandle_t* deviceHandlePtr;
struct ncGraphHandle_t* graphHandlePtr_seg;
struct ncGraphHandle_t* graphHandlePtr_det;
void* graphFileBuf_seg;
void* graphFileBuf_det;
unsigned int graphFileLenSeg;
unsigned int graphFileLenDet;
// Now we need to allocate graph and create and in/out fifos
struct ncFifoHandle_t* inFifoHandlePtr_seg;
struct ncFifoHandle_t* outFifoHandlePtr_seg;
struct ncFifoHandle_t* inFifoHandlePtr_det;
struct ncFifoHandle_t* outFifoHandlePtr_det;

int numClasses_;
float ssd_threshold;

std::map<int, std::string> LABELS = {{0,"background"}, {1, "person"}, {2, "car"}, {3, "bicycle"}};

image* ipl_to_image(IplImage* src)
{
    int h = src->height;
    int w = src->width;
    int c = src->nChannels;

    image* out = new image(w,h,c);

    ipl_into_image(src, out);
    return out;
}


void ipl_into_image(IplImage* src, image* im)
{
//  unsigned char *data = (unsigned char *)src->imageData;
    int h = src->height;
    int w = src->width;
    int c = src->nChannels;
    int step = src->widthStep;
    int i, j, k;
    // std::cout << "h: " << h << " w: " << w << " c: " << c << std::endl;

    for(i = 0; i < h; ++i){
        for(k= 0; k < c; ++k){
            for(j = 0; j < w; ++j){
                im->data[k*w*h + i*w + j] = ((unsigned char *)src->imageData)[i*step + j*c + k]/255.;
            }
        }
    }
}


unsigned char* image_to_stb(image* in)
{
    int i,j,k;
    int w = in->w;
    int h = in->h;
    int c =3;
    unsigned char *img = (unsigned char*) malloc(c*w*h);
    for(k = 0; k < c; ++k){
        for(j=0; j <h; ++j){
            for(i=0; i<w; ++i){
                int src_index = i + w*j + w*h*k;
                int dst_index = k + c*i + c*w*j;
                img[dst_index] = (unsigned char)(255*in->data[src_index]);
            }
        }
    }
    delete in;
    // std::cout << "xxxxx" << std::endl;
    return img;
}



unsigned char* cvMat_to_charImg(cv::Mat pic)
{
    IplImage copy = pic;
    IplImage *pic_Ipl = &copy;

    image* buff_;
    buff_ = ipl_to_image(pic_Ipl);
    unsigned char* pic_final = image_to_stb(buff_);

    //cvReleaseImage(&pic_Ipl);

    return pic_final;
}

// Load a graph file, caller must free the buffer returned.
// path is the full or relative path to the graph file, length is the number of bytes read upon return
void *LoadFile(const char *path, unsigned int *length)
{
    FILE *fp;
    char *buf;

    fp = fopen(path, "rb");
    if(fp == NULL)
        return 0;
    fseek(fp, 0, SEEK_END);
    *length = ftell(fp);
    rewind(fp);
    if(!(buf = (char*) malloc(*length)))
    {
        fclose(fp);
        return 0;
    }
    if(fread(buf, 1, *length, fp) != *length)
    {
        fclose(fp);
        free(buf);
        return 0;
    }
    fclose(fp);
    return buf;
}

// load the image converted from ros topic, resize and normalize the image with type float32
float *LoadImage32(unsigned char *img, int target_w, int target_h, int ori_w, int ori_h, float *mean)
{
    int i;
    unsigned char *imgresized;
    float *imgfp32;

    if(!img)
    {
        printf("The picture  could not be loaded\n");
        return 0;
    }
    imgresized = (unsigned char*) malloc(3*target_w*target_h);
    if(!imgresized)
    {
        free(img);
        perror("malloc");
        return 0;
    }
    stbir_resize_uint8(img, ori_w, ori_h, 0, imgresized, target_w, target_h, 0, 3);
    free(img);
    imgfp32 = (float*) malloc(sizeof(*imgfp32) * target_w * target_h * 3);
    if(!imgfp32)
    {
        free(imgresized);
        perror("malloc");
        return 0;
    }
    for(i = 0; i < target_w * target_h * 3; i++)
    {
        imgfp32[i] = imgresized[i];
    }
    free(imgresized);

    for(i = 0; i < target_w*target_h; i++)
    {
        // imgfp32 comes in RGB order but network expects to be in
        // BRG order so convert to BGR here while subtracting the mean.
        float blue, green, red;
        blue = imgfp32[3*i+2];
        green = imgfp32[3*i+1];
        red = imgfp32[3*i+0];

        imgfp32[3*i+0] = (blue-mean[0]) * 0.007843;
        imgfp32[3*i+1] = (green-mean[1]) * 0.007843;
        imgfp32[3*i+2] = (red-mean[2]) * 0.007843;

        // uncomment to see what values are getting passed to mvncLoadTensor() before conversion to half float
        //printf("Blue: %f, Grean: %f,  Red: %f \n", imgfp32[3*i+0], imgfp32[3*i+1], imgfp32[3*i+2]);
    }
    return imgfp32;
}


//convert movidius seg_output to mask image
cv::Mat seg_result_process(float *output, int h, int w)
{
    cv::Mat mask_gray(h, w, CV_8UC1);
    cv::Mat mask;

    for (int i = 0; i < h; ++i) {
        for (int j = 0; j < w; ++j) {
            if(output[2*(w*i + j)] < output[2*(w*i + j) + 1]){
                mask_gray.at<uchar>(i,j) = 255;
            } else{
                mask_gray.at<uchar>(i,j) = 0;
            }
        }
    }
    // gray -> color
    cv::cvtColor(mask_gray, mask, cv::COLOR_GRAY2BGR);

    return mask;
}

//  a.	First fp16 value holds the number of valid detections = num_valid.
//  b.	The next 6 values are unused.
//  c.	The next (7 * num_valid) values contain the valid detections data
//      Each group of 7 values will describe an object/box These 7 values in order.
//       The values are:
//         0: image_id (always 0)
//         1: class_id (this is an index into labels)
//         2: score (this is the probability for the class)
//         3: box left location within image as number between 0.0 and 1.0
//         4: box top location within image as number between 0.0 and 1.0
//         5: box right location within image as number between 0.0 and 1.0
//         6: box bottom location within image as number between 0.0 and 1.0
// reslut 解析
void ssd_result_process(float *output, std::vector<Box> &result, cv::Mat &image, int numClasses_){
    int num_valid_boxes = output[0];

    std::cout<< "num_valid_boxes: " << num_valid_boxes <<std::endl;

    std::vector<Box> Rects_with_labels[numClasses_];
    //std::cout << "num_valid_boxes: " << num_valid_boxes << std::endl;
    // clip the boxes to the image size incase network returns boxes outside of the image
    for (int box_index = 0; box_index < num_valid_boxes; box_index++) {
        int base_index = 7 + box_index * 7;
        //todo_ziwei: 省略判断每一组数值是否有效(inf, nan, etc)， 之后需补上
        float x1 =
                0 > (int) (output[base_index + 3] * image.rows) ? 0 : (int) (output[base_index + 3] * image.rows);
        float y1 =
                0 > (int) (output[base_index + 4] * image.cols) ? 0 : (int) (output[base_index + 4] * image.cols);
        float x2 = image.rows < (int) (output[base_index + 5] * image.rows) ? image.rows : (int) (
                output[base_index + 5] * image.rows);
        float y2 = image.cols < (int) (output[base_index + 6] * image.cols) ? image.cols : (int) (
                output[base_index + 6] * image.cols);

//        std::cout << "box at index: " << box_index << " ClassID: " << LABELS[(int)output[base_index + 1]]<< " Confidence: " << output[base_index + 2]
//                  << " Top Left: " << x1 << "," << y1 << " Bottom Right:" << x2 << "," << y2 << std::endl;

        //用于图像显示box和label
        float output_pass[7];
        for (int i = 0; i < 7; i++) {
            output_pass[i] = output[base_index + i];
        }

        Box single_box;
        if (Overlay_on_image(image, output_pass, 7, single_box))
        {
            int label_idx = single_box.label;
            Rects_with_labels[label_idx].push_back(single_box);
            //result.push_back(single_box); //Rects_with_labels
        }
    }

    // NMS and visualize; insert the bbox to result vector
    for(int i =0; i < numClasses_; i++)
    {
//        NMS(Rects_with_labels[i]);
        for(int j = 0; j<Rects_with_labels[i].size(); j++)
        {
            cv::Rect box;
            box.x = Rects_with_labels[i][j].x;
            box.y = Rects_with_labels[i][j].y;
            box.width = Rects_with_labels[i][j].width;
            box.height = Rects_with_labels[i][j].height;
            int red_level = (int) (255.0 / LABELS.size()) * Rects_with_labels[i][j].label;
            std::string label = LABELS[(int) Rects_with_labels[i][j].label];
            cv::rectangle(image, box, cv::Scalar(red_level, 255, 255), 2);
            cv::putText(image, label,
                        cv::Point(box.x, box.y),
                        cv::FONT_HERSHEY_COMPLEX_SMALL, 1, cv::Scalar(red_level, 255, 255), 0.4, CV_AA);
            result.push_back(Rects_with_labels[i][j]);
        }
    }

}

////filter some boxes of which score lower than threshold
bool Overlay_on_image(cv::Mat &image, float *object_info, int Length, Box &single_box) {
    // int min_score_percent = 60;
    int min_score_percent = int(ssd_threshold * 100);
    int source_image_width = image.cols;
    int source_image_height = image.rows;
    int base_index = 0;
    int class_id = object_info[base_index + 1];
    int percentage = int(object_info[base_index + 2] * 100);
    if (percentage < min_score_percent)
        return false;

//        std::cout << "source_image_width: " << source_image_width << " source_image_height: " << source_image_height
//                  << std::endl;
    int box_left = (int) (object_info[base_index + 3] * source_image_width);
    int box_top = (int) (object_info[base_index + 4] * source_image_height);
    int box_right = (int) (object_info[base_index + 5] * source_image_width);
    int box_bottom = (int) (object_info[base_index + 6] * source_image_height);
    int box_width = box_right - box_left;
    int box_height = box_bottom - box_top;
    cv::Rect box;
    box.x = box_left;
    box.y = box_top;
    box.width = box_width;
    box.height = box_height;

    int label_index = (int) object_info[base_index + 1];
    std::string label = LABELS[(int) object_info[base_index + 1]];
    int red_level = (int) (255.0 / LABELS.size()) * label_index;
    if(label_index != 0)
    {
        cv::rectangle(image, box, cv::Scalar(red_level, 255, 255), 2);
        cv::putText(image, label,
                    cv::Point(box.x, box.y),
                    cv::FONT_HERSHEY_COMPLEX_SMALL, 1, cv::Scalar(red_level, 255, 255), 1, CV_AA);
    }
//        std::cout << "box at index: " << label_index << " ClassID: " << LABELS[(int) object_info[base_index + 1]]
//                  << " Confidence: " << object_info[base_index + 2]
//                  << " Top Left: " << box_top << "," << box_left << " Bottom Right:" << box_right << "," << box_bottom
//                  << std::endl;
    single_box.label = label_index;
    single_box.x = box_left;
    single_box.y = box_top;
    single_box.width = box_width;
    single_box.height = box_height;
    single_box.prob = object_info[base_index + 2];
    return true;
}

void NMS(std::vector <Box> &M)
{
    sizeSort(M);
    for(int i=0; i<M.size(); i++)
    {
        for(int j=i+1; j<M.size(); j++)
        {
            cv::Rect a, b;
            a.x = M[i].x;
            a.y = M[i].y;
            a.width = M[i].width;
            a.height = M[i].height;

            b.x = M[j].x;
            b.y = M[j].y;
            b.width = M[j].width;
            b.height = M[j].height;
            //std::cout << "getOverlap(a, b): " << getOverlap(a, b) << std::endl;
            if(getOverlap(a, b) > 0.7) //同类框的重复面积大于0.73需删除
            {
                M.erase(M.begin()+j);
                j--;
                // std::cout << "delete a yolo rect" << std::endl;
            }
        }
    }
}

void sizeSort(std::vector <Box> &M)
{
    sort(M.begin(),M.end(),[](const Box &a, const Box &b)
    {
        return a.width*a.height > b.width*b.height;
    });
}

inline float getOverlap(const cv::Rect &b1, const cv::Rect &b2) //b1是激光的框，b2是摄像头识别的人
{
#define min___(a, b) (a > b ? b : a)
#define max___(a, b) (a < b ? b : a)
    int ws1 = min___(b1.x + b1.width, b2.x + b2.width) - max___(b1.x, b2.x);
    int hs1 = min___(b1.y + b1.height, b2.y + b2.height) - max___(b1.y, b2.y);
    float o = max___(0, ws1) * max___(0, hs1);
    o = o / (b2.width * b2.height);
    //o = o / (b1.width * b1.height + b2.width * b2.height - o);
    return o;
}


double getWallTime() {
    struct timeval time;
    if (gettimeofday(&time, NULL)) {
        return 0;
    }
    return (double) time.tv_sec + (double) time.tv_usec * .000001;
}
