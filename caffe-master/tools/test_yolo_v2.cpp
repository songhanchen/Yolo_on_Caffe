#include <caffe/caffe.hpp>
#include <caffe/data_transformer.hpp>
#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <omp.h>
#include <boost/shared_ptr.hpp>
#include <algorithm>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <time.h>

using namespace caffe;
using namespace cv;
using namespace std;

typedef struct{
	float x;
	float y;
	float w;
	float h;
	float score;
	int class_index;
} yolobox;

typedef struct
{
	int index;
	float probvalue;
}sortable_element;

typedef struct
{
	float w;
	float h;
}anchor_unit;

template <typename Dtype>
inline Dtype sigmoidfunc(Dtype x)
{
	return 1. / (1. + exp(-x));
}

static float anchors_array[10] = { 1.3221, 1.73145, 3.19275, 4.00944, 5.05587, 8.09892, 9.47112, 4.84053, 11.2364, 10.0071 };

template <typename Dtype>
Dtype softmax_region(Dtype* input, int classes)
{
	Dtype sum = 0;
	Dtype large = input[0];
	for (int i = 0; i < classes; ++i)
	{
		//printf("class prob is %f\n", input[i]);
		if (input[i] > large)
			large = input[i];
	}
	for (int i = 0; i < classes; ++i){
		Dtype e = exp(input[i] - large);
		sum += e;
		input[i] = e;
	}
	for (int i = 0; i < classes; ++i){
		input[i] = input[i] / sum;
	}
	return 0;
}

//need to create a class for detection
//the certain class should include the functions based on caffe.lib to solve the detection problems
class YOLOV2Detect
{
public:
	YOLOV2Detect();
	~YOLOV2Detect();
public:
	YOLOV2Detect(const string& proto_model_dir);
	std::vector<yolobox> YoloV2RegionDetect(const cv::Mat& testimage);
	std::vector<yolobox> Do_NMSMultiLabel(std::vector<yolobox>& regboxes, std::vector<std::vector<float>>& probs, float thresh, float thresh_iou);
	yolobox get_region_box(float *x, vector<anchor_unit> biases, int n, int index, int i, int j, int w, int h);
	void WrapInputLayer(std::vector<cv::Mat>* input_channels);
	void Preprocess(const cv::Mat& img, std::vector<cv::Mat>* input_channels);
public:
	float Boxoverlap(float x1, float w1, float x2, float w2);
	float Calc_iou(const yolobox& box1, const yolobox& box2);
private:
	float *object_prob_;
	float *class_prob_;
	std::vector<std::vector<float>> fprobs_;
	boost::shared_ptr<Net<float>> Yolov2Net_;
    //transform data
	TransformationParameter transform_param_;
	boost::shared_ptr<DataTransformer<float>> data_transformer_;
	Blob<float> transformed_data_;
	//yolov2 anchor boxes
	vector<anchor_unit> anchor_vec;
	int num_channels_;
	int num_class_;
	int sidenum_;
	int location_;
	int num_objects_;
	cv::Size Input_geometry_;
	clock_t time_start_;
	clock_t time_end_;
	std::vector<yolobox> result_boxes_;
	std::vector<yolobox> total_boxes_;
	//thresh
	float thresh_score_;
	float thresh_iou_;
	//mean_value
	float mean_value_[3];
};


YOLOV2Detect::YOLOV2Detect()
{

}

YOLOV2Detect::~YOLOV2Detect()
{

}

YOLOV2Detect::YOLOV2Detect(const string& proto_model_dir)
{
	Caffe::set_mode(Caffe::CPU);
	Yolov2Net_.reset(new Net<float>((proto_model_dir + "/yolov2deploy.prototxt"), caffe::TEST));
	Yolov2Net_->CopyTrainedLayersFrom(proto_model_dir + "/yolov2.caffemodel");

	Blob<float>* input_layer = Yolov2Net_->input_blobs()[0];
	num_channels_ = input_layer->channels();
	Input_geometry_.width = input_layer->width();
	Input_geometry_.height = input_layer->height();
	num_class_ = 20;
	sidenum_ = 13;
	num_objects_ = 5;
	location_ = pow(sidenum_, 2);
	object_prob_ = (float *)malloc(num_objects_*location_*sizeof(float));
	class_prob_ = (float *)malloc(num_class_*location_*num_objects_*sizeof(float));
	thresh_score_ = 0.25f;
	thresh_iou_ = 0.70f;
	int anchor_size = sizeof(anchors_array)/sizeof(float);
	anchor_vec.resize(anchor_size/2);
	fprobs_.resize(num_objects_*location_);
	for (int i = 0; i < fprobs_.size(); i++)
	{
		fprobs_[i].resize(num_class_);
	}
	for (int i = 0; i < anchor_size/2; i++)
	{
		anchor_vec[i].w = anchors_array[2*i];
		anchor_vec[i].h = anchors_array[2*i+1];
	}
	for (int i = 0; i < 3; i++)
	{
		mean_value_[i] = 0;
	}
}

float YOLOV2Detect::Boxoverlap(float x1, float w1, float x2, float w2)
{
	float left = std::max(x1 - w1 / 2, x2 - w2 / 2);
	float right = std::min(x1 + w1 / 2, x2 + w2 / 2);
	return right - left;
}

float YOLOV2Detect::Calc_iou(const yolobox& box1, const yolobox& box2)
{
	float w = Boxoverlap(box1.x, box1.w, box2.x, box2.w);
	float h = Boxoverlap(box1.y, box1.h, box2.y, box2.h);
	if (w < 0 || h < 0) return 0;
	float inter_area = w * h;
	float union_area = box1.w * box1.h + box2.w * box2.h - inter_area;
	return inter_area / union_area;
}

bool CompareYoloBBox(const sortable_element & a, const sortable_element & b) {
	if (a.probvalue == b.probvalue)
		return a.index > b.index;
	else
		return a.probvalue > b.probvalue;
}

yolobox YOLOV2Detect::get_region_box(float *x, vector<anchor_unit> biases, int n, int index, int i, int j, int w, int h)
{
	/*printf("x is %f\n", x[index + 0]);
	printf("y is %f\n", x[index + 1]);
	printf("w is %f\n", x[index + 2]);
	printf("h is %f\n", x[index + 3]);
	printf("score is %f\n", x[index + 4]);*/
	yolobox result_vec;
	result_vec.x = ((float)j + sigmoidfunc<float>(x[index + 0])) / w;
	result_vec.y = ((float)i + sigmoidfunc<float>(x[index + 1])) / h;
	result_vec.w = (exp(x[index + 2])*biases[n].w) / w;
	result_vec.h = (exp(x[index + 3])*biases[n].h) / h;
	return result_vec;
}

std::vector<yolobox> YOLOV2Detect::Do_NMSMultiLabel(std::vector<yolobox>& regboxes, std::vector<std::vector<float>>& probs, float thresh, float threshiou)
{
	std::vector<yolobox> bboxes_nms;
	std::vector<sortable_element> sortelements;
	int totalnum = regboxes.size();
	assert(totalnum == probs.size());
	sortelements.resize(totalnum);
	//init
	for (int c = 0; c < num_class_; c++)
	{
		for (int k = 0; k < totalnum; k++)
		{
			sortelements[k].index = k;
			sortelements[k].probvalue = probs[k][c];
			if (probs[k][c] > 0.0f)
			{
				printf("probs is %f\n", probs[k][c]);
			}
		}
		std::sort(sortelements.begin(), sortelements.end(), CompareYoloBBox);
		for (int i = 0; i < sortelements.size(); ++i)
		{
			if (probs[sortelements[i].index][c] < thresh)
			{
				probs[sortelements[i].index][c] = 0.0f;
				continue;
			}
			yolobox a = regboxes[sortelements[i].index];
			for (int j = i + 1; j < totalnum; ++j)
			{
				yolobox b = regboxes[sortelements[j].index];
				float calced_iou = Calc_iou(a, b);
				if (calced_iou > threshiou)
				{
					probs[sortelements[j].index][c] = 0.0f;
				}
			}
		}
	}
	for (int c = 0; c < num_class_; c++)
	{
		for (int k = 0; k < totalnum; k++)
		{
			if (probs[k][c] == 0.0f)
				continue;
			regboxes[k].class_index = c;
			bboxes_nms.push_back(regboxes[k]);
		}
	}
	//float calced_iou111 = Calc_iou(bboxes_nms[0], bboxes_nms[1]);
	return bboxes_nms;
}

/* Wrap the input layer of the network in separate cv::Mat objects
* (one per channel). This way we save one memcpy operation and we
* don't need to rely on cudaMemcpy2D. The last preprocessing
* operation will write the separate channels directly to the input
* layer. */
void YOLOV2Detect::WrapInputLayer(std::vector<cv::Mat>* input_channels)
{
	Blob<float>* input_layer = Yolov2Net_->input_blobs()[0];

	int width = input_layer->width();
	int height = input_layer->height();
	float* input_data = input_layer->mutable_cpu_data();
	for (int i = 0; i < input_layer->channels(); ++i)
	{
		cv::Mat channel(height, width, CV_32FC1, input_data);
		input_channels->push_back(channel);
		input_data += width * height;
	}
}

void YOLOV2Detect::Preprocess(const cv::Mat& img, std::vector<cv::Mat>* input_channels)
{
	/* Convert the input image to the input image format of the network. */
	cv::Mat sample;
	if (img.channels() == 3 && num_channels_ == 1)
		cv::cvtColor(img, sample, cv::COLOR_BGR2GRAY);
	else if (img.channels() == 4 && num_channels_ == 1)
		cv::cvtColor(img, sample, cv::COLOR_BGRA2GRAY);
	else if (img.channels() == 4 && num_channels_ == 3)
		cv::cvtColor(img, sample, cv::COLOR_BGRA2BGR);
	else if (img.channels() == 1 && num_channels_ == 3)
		cv::cvtColor(img, sample, cv::COLOR_GRAY2BGR);
	else
		sample = img;

	cv::Mat sample_resized;
	if (sample.size() != Input_geometry_)
		cv::resize(sample, sample_resized, Input_geometry_);
	else
		sample_resized = sample;

	cv::Mat sample_float;
	if (num_channels_ == 3)
		sample_resized.convertTo(sample_float, CV_32FC3);
	else
		sample_resized.convertTo(sample_float, CV_32FC1);

	cv::Mat sample_normalized;
	//cv::subtract(sample_float, mean_, sample_normalized);
	sample_float.copyTo(sample_normalized);

	/* This operation will write the separate BGR planes directly to the
	* input layer of the network because it is wrapped by the cv::Mat
	* objects in input_channels. */
	cv::split(sample_normalized, *input_channels);

	CHECK(reinterpret_cast<float*>(input_channels->at(0).data)
		== Yolov2Net_->input_blobs()[0]->cpu_data())
		<< "Input channels are not wrapping the input layer of the network.";
}

std::vector<yolobox> YOLOV2Detect::YoloV2RegionDetect(const cv::Mat& testimage)
{
	int width = Input_geometry_.width;
	int height = Input_geometry_.height;
	float *tempclassarray = (float *)malloc(num_class_*sizeof(float));
	Blob<float>* input_layer = Yolov2Net_->input_blobs()[0];
	input_layer->Reshape(1, num_channels_, Input_geometry_.height, Input_geometry_.width);
	Yolov2Net_->Reshape();
	//transform_param_.
	/*std::vector<cv::Mat> input_channels;
	WrapInputLayer(&input_channels);
	Preprocess(testimage, &input_channels);*/

	float *input_data = input_layer->mutable_cpu_data();
	cv::Mat sample_resized;
	if (testimage.size() != Input_geometry_)
		cv::resize(testimage, sample_resized, Input_geometry_);
	else
		sample_resized = testimage;
	cv::Vec3b *img_data = (cv::Vec3b *)sample_resized.data;
	int spatial_size = width*height;
	for (int k = 0; k < spatial_size; ++k)
	{
		input_data[k] = float(img_data[k][2] - mean_value_[0])/255.0f;
		input_data[k + spatial_size] = float(img_data[k][1] - mean_value_[1])/255.0f;
		input_data[k + 2 * spatial_size] = float(img_data[k][0] - mean_value_[2])/255.0f;
	}
	Yolov2Net_->Forward();

#ifdef DEBUG_LAYER
	vector<boost::shared_ptr<Layer<float>>> layers = Yolov2Net_->layers();
	int layersize = layers.size();
	char resultlayerfile[256] = {0};
	vector< string > const & blob_names = Yolov2Net_->blob_names();
	int blob_nums = blob_names.size();
	for (int i = 1; i < blob_nums; i++)
	{
		//string layer_cname = layers[i]->layer_param().name();
		boost::shared_ptr<Blob<float>> layer_blob = Yolov2Net_->blob_by_name(blob_names[i]);
		const float *blob_data;
		int batch_size = layer_blob->num();
		int batch_size1 = 1;
		int layerwidth = layer_blob->width();
		int layerheight = layer_blob->height();
		int layerchannels = layer_blob->channels();
		int dim_features = layer_blob->count() / batch_size;
		int calced_dimfeature = layerwidth*layerheight*layerchannels;
		printf("batch_size is %d\n", batch_size);
		printf("width is %d\n", layerwidth);
		printf("height is %d\n", layerheight);
		printf("channels is %d\n", layerchannels);
		for (int bn = 0; bn < batch_size; bn++)
		{
			blob_data = layer_blob->cpu_data() + layer_blob->offset(bn);
			sprintf(resultlayerfile, "D:/python/python_project/caffe_result/layer_%s_bn%d.dat", blob_names[i].c_str(), bn + 1);
			FILE *fp = NULL;
			fopen_s(&fp, resultlayerfile, "wb");
			fwrite(blob_data, sizeof(float), calced_dimfeature, fp);
			fclose(fp);
		}
	}
#endif

	boost::shared_ptr<Blob<float>> layer_blobf = Yolov2Net_->blob_by_name("conv23");
	printf("output batch is %d\n", layer_blobf->num());
	printf("output width is %d\n", layer_blobf->width());
	printf("output height is %d\n", layer_blobf->height());
	printf("output channel is %d\n", layer_blobf->channels());
	int batchnum = layer_blobf->num();
	int owidth = layer_blobf->width();
	int oheight = layer_blobf->height();
	int ochannels = layer_blobf->channels();
	const float *output_data = layer_blobf->cpu_data();

	//rerange the output_data from (n,c,h,w) -> (n, h, w, c)
	float *out_rerange = (float *)malloc(batchnum*owidth*oheight*ochannels*sizeof(float));
	int simagestep = owidth*oheight;
	int reimagestep = oheight*owidth*ochannels;
	int colstep = owidth*ochannels;
	for (int nc = 0; nc < batchnum; nc++)
	{
		for (int h = 0; h < oheight; h++)
		{
			for (int w = 0; w < owidth; w++)
			{
				for (int c = 0; c < ochannels; c++)
				{
					out_rerange[nc*reimagestep + h*colstep + w*ochannels + c] = output_data[nc*reimagestep + c*simagestep + h*owidth + w];
				}
			}
		}
	}
	//rerange the output data from (n,c,h,w) -> (n, h, w, c)
	int blob_num = layer_blobf->num();
	int blob_insidecount = layer_blobf->count(1);
	int feature_step = num_class_ + 1 + 4;
	int class_step = num_class_;
	for (int k = 0; k < layer_blobf->num(); ++k)
	{
		std::vector<yolobox> result_boxes;
		int index = k * blob_insidecount;
		float *temp_ptr = &out_rerange[index];
		for (int i = 0; i < sidenum_; ++i)
		{
			for (int j = 0; j < sidenum_; ++j)
			{
				for (int n = 0; n < num_objects_; n++)
				{
					int index = i*sidenum_*feature_step*num_objects_ + j*num_objects_*feature_step + n*feature_step;
					int c_index = class_step*(i*sidenum_*num_objects_ + j*num_objects_ + n);
					int p_index = i*sidenum_*num_objects_ + j*num_objects_ + n;
					yolobox tempbox = get_region_box(temp_ptr, anchor_vec, n, index, i, j, sidenum_, sidenum_);
					tempbox.score = temp_ptr[index + 4];
					result_boxes.push_back(tempbox);
					tempclassarray = &temp_ptr[index + 5];
					float temp_probeclass = softmax_region(tempclassarray, num_class_);
					for (int c = 0; c < num_class_; c++)
					{
						class_prob_[c_index + c] = sigmoidfunc(tempbox.score)*tempclassarray[c];
						if (class_prob_[c_index + c] >= thresh_score_)
						{
							printf("above thresh!\n");
						}
						fprobs_[p_index][c] = (class_prob_[c_index + c] < thresh_score_) ? 0.0f : class_prob_[c_index + c];
					}
				}
			}
		}
		std::vector<yolobox> bboxes_nms = Do_NMSMultiLabel(result_boxes, fprobs_, thresh_score_, thresh_iou_);
		if (bboxes_nms.size() > 0) {
			total_boxes_.insert(total_boxes_.end(), bboxes_nms.begin(), bboxes_nms.end());
		}
	}
	return total_boxes_;
}


int main(int argc, char **argv)
{
	string root = "D:/Code_local/caffe_yolov2_windows/net_work_train";
	string modelroot = "D:/Code_local/caffe_yolov2_windows/net_work_train";
	string testimagefile = "D:/Code_local/caffe_yolov2_windows/net_work_train/horses.jpg";
	cv::Mat testimage = imread(testimagefile);
	YOLOV2Detect yolodetector(modelroot);
	double t = (double)cv::getTickCount();
	std::vector<yolobox> detected_box = yolodetector.YoloV2RegionDetect(testimage);
	std::cout << " time," << (double)(cv::getTickCount() - t) / cv::getTickFrequency() << "s" << std::endl;
	for (int i = 0; i < detected_box.size(); i++){
		int x = (int)((detected_box[i].x - detected_box[i].w / 2)*testimage.cols);
		int y = (int)((detected_box[i].y - detected_box[i].h / 2)*testimage.rows);
		int w = (int)((detected_box[i].w)*testimage.cols);
		int h = (int)((detected_box[i].h)*testimage.rows);
		cv::rectangle(testimage, cv::Rect(x, y, w, h), cv::Scalar(255, 0, 0), 2);
	}
	imwrite("D:/Code_local/caffe/caffe_yolo/debug_result/yolotestresult.jpg", testimage);
	FILE *fp = 0;
	fopen_s(&fp, "D:/Code_local/caffe/caffe_yolo/debug_result/box_info.txt", "w+");
	for (int i = 0; i < detected_box.size(); i++)
	{
		fprintf(fp, "%d  ", detected_box[i].class_index);
		fprintf(fp, "%f  ", detected_box[i].x);
		fprintf(fp, "%f  ", detected_box[i].y);
		fprintf(fp, "%f  ", detected_box[i].w);
		fprintf(fp, "%f  ", detected_box[i].h);
		fprintf(fp, "\n");
	}
	fclose(fp);
	return 1;
}