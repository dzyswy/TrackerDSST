/*

Tracker based on Kernelized Correlation Filter (KCF) [1] and Circulant Structure with Kernels (CSK) [2].
CSK is implemented by using raw gray level features, since it is a single-channel filter.
KCF is implemented by using HOG features (the default), since it extends CSK to multiple channels.

[1] J. F. Henriques, R. Caseiro, P. Martins, J. Batista,
"High-Speed Tracking with Kernelized Correlation Filters", TPAMI 2015.

[2] J. F. Henriques, R. Caseiro, P. Martins, J. Batista,
"Exploiting the Circulant Structure of Tracking-by-detection with Kernels", ECCV 2012.

Authors: Joao Faro, Christian Bailer, Joao F. Henriques
Contacts: joaopfaro@gmail.com, Christian.Bailer@dfki.de, henriques@isr.uc.pt
Institute of Systems and Robotics - University of Coimbra / Department Augmented Vision DFKI


Constructor parameters, all boolean:
    hog: use HOG features (default), otherwise use raw pixels
    fixed_window: fix window size (default), otherwise use ROI size (slower but more accurate)
    multiscale: use multi-scale tracking (default; cannot be used with fixed_window = true)

Default values are set for all properties of the tracker depending on the above choices.
Their values can be customized further before calling init():
    interp_factor: linear interpolation factor for adaptation
    sigma: gaussian kernel bandwidth
    lambda: regularization
    cell_size: HOG cell size
    padding: area surrounding the target, relative to its size
    output_sigma_factor: bandwidth of gaussian target
    template_size: template size in pixels, 0 to use ROI size
    scale_step: scale step for multi-scale estimation, 1 to disable it
    scale_weight: to downweight detection scores of other scales for added stability

For speed, the value (template_size/cell_size) should be a power of 2 or a product of small prime numbers.

Inputs to init():
   image is the initial frame.
   roi is a cv::Rect with the target positions in the initial frame

Inputs to update():
   image is the current frame.

Outputs of update():
   cv::Rect with target positions for the current frame


By downloading, copying, installing or using the software you agree to this license.
If you do not agree to this license, do not download, install,
copy or use the software.


                          License Agreement
               For Open Source Computer Vision Library
                       (3-clause BSD License)

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the names of the copyright holders nor the names of the contributors
    may be used to endorse or promote products derived from this software
    without specific prior written permission.

This software is provided by the copyright holders and contributors "as is" and
any express or implied warranties, including, but not limited to, the implied
warranties of merchantability and fitness for a particular purpose are disclaimed.
In no event shall copyright holders or contributors be liable for any direct,
indirect, incidental, special, exemplary, or consequential damages
(including, but not limited to, procurement of substitute goods or services;
loss of use, data, or profits; or business interruption) however caused
and on any theory of liability, whether in contract, strict liability,
or tort (including negligence or otherwise) arising in any way out of
the use of this software, even if advised of the possibility of such damage.
 */

#ifndef _KCFTRACKER_HEADERS
#include "kcftracker.hpp"
#include "ffttools.hpp"
#include "recttools.hpp"
#include "fhog.hpp"
#include "labdata.hpp"
#endif

// Constructor
KCFTracker::KCFTracker(bool hog, bool fixed_window, bool multiscale, bool lab)
{

    // Parameters equal in all cases
    lambda = 0.0001;
    padding = 2.5;
    //output_sigma_factor = 0.1;
    output_sigma_factor = 0.125;

    if (hog) {    // HOG
        // VOT
        interp_factor = 0.012;
        sigma = 0.6;
        // TPAMI
        //interp_factor = 0.02;
        //sigma = 0.5;
        cell_size = 4;
        _hogfeatures = true;

        if (lab) {
            interp_factor = 0.005;
            sigma = 0.4;
            //output_sigma_factor = 0.025;
            output_sigma_factor = 0.1;

            _labfeatures = true;
            _labCentroids = cv::Mat(nClusters, 3, CV_32FC1, &data);
            cell_sizeQ = cell_size*cell_size;
        }
        else{
            _labfeatures = false;
        }
    }
    else {   // RAW
        interp_factor = 0.075;
        sigma = 0.2;
        cell_size = 1;
        _hogfeatures = false;

        if (lab) {
            printf("Lab features are only used with HOG features.\n");
            _labfeatures = false;
        }
    }


    if (multiscale) { // multiscale
        template_size = 96;
        //scale parameters initial
        scale_padding = 1.0;
        scale_step = 1.05;
        scale_sigma_factor = 0.25;
        n_scales = 33;
        scale_lr = 0.025;
        scale_max_area = 512;
        currentScaleFactor = 1;
        scale_lambda = 0.01;

        if (!fixed_window) {
            //printf("Multiscale does not support non-fixed window.\n");
            fixed_window = true;
        }
    }
    else if (fixed_window) {  // fit correction without multiscale
        template_size = 96;
        //template_size = 100;
        scale_step = 1;
    }
    else {
        template_size = 1;
        scale_step = 1;
    }
}


// Destructor
KCFTracker::~KCFTracker()
{
   printf("KCFTracker Destructor.\n");

   _alphaf.release();
   _prob.release();
   _tmpl.release();
   _num.release();
   _den.release();
   _labCentroids.release();
   sf_den.release();
   sf_num.release();
   
   hann.release();
   s_hann.release();
   ysf.release();
}


// Initialize tracker
void KCFTracker::init(const cv::Rect &roi, cv::Mat image)
{
    _roi = roi;
    assert(roi.width >= 0 && roi.height >= 0);
    _tmpl = getFeatures(image, 1);
    _prob = createGaussianPeak(size_patch[0], size_patch[1]);
    _alphaf = cv::Mat(size_patch[0], size_patch[1], CV_32FC2, float(0));

    dsstInit(roi, image);
    //_num = cv::Mat(size_patch[0], size_patch[1], CV_32FC2, float(0));
    //_den = cv::Mat(size_patch[0], size_patch[1], CV_32FC2, float(0));
    train(_tmpl, 1.0); // train with initial frame
 }

// Initialize tracker
void KCFTracker::init(const cv::Point pt1, const cv:: Point pt2, cv::Mat image)
{

    cv::Rect targetRect; 
    targetRect.x = MIN(pt1.x, pt2.x);
    targetRect.y = MIN(pt1.y, pt2.y);
    targetRect.width = abs(pt2.x - pt1.x);
    targetRect.height = abs(pt2.y - pt1.y);
    targetRect&=cv::Rect(0, 0, image.cols, image.rows);

    init(targetRect, image);
}

// Update position based on the new frame
cv::Rect KCFTracker::update(cv::Mat image)
{
    if (_roi.x + _roi.width <= 0) _roi.x = -_roi.width + 1;
    if (_roi.y + _roi.height <= 0) _roi.y = -_roi.height + 1;
    if (_roi.x >= image.cols - 1) _roi.x = image.cols - 2;
    if (_roi.y >= image.rows - 1) _roi.y = image.rows - 2;

    float cx = _roi.x + _roi.width / 2.0f;
    float cy = _roi.y + _roi.height / 2.0f;

    float peak_value;
    cv::Point2f res = detect(_tmpl, getFeatures(image, 0, 1.0f), peak_value);

    //printf("Peak value: %f\n", peak_value);

    // Adjust by cell size and _scale
    _roi.x = cx - _roi.width / 2.0f + ((float) res.x * cell_size * _scale * currentScaleFactor);
    _roi.y = cy - _roi.height / 2.0f + ((float) res.y * cell_size * _scale * currentScaleFactor);

    if (_roi.x >= image.cols - 1) _roi.x = image.cols - 1;
    if (_roi.y >= image.rows - 1) _roi.y = image.rows - 1;
    if (_roi.x + _roi.width <= 0) _roi.x = -_roi.width + 2;
    if (_roi.y + _roi.height <= 0) _roi.y = -_roi.height + 2;

    // Update scale
    cv::Point2i scale_pi = detect_scale(image);
    currentScaleFactor = currentScaleFactor * scaleFactors[scale_pi.x];
    if(currentScaleFactor < min_scale_factor)
      currentScaleFactor = min_scale_factor;
    // else if(currentScaleFactor > max_scale_factor)
    //   currentScaleFactor = max_scale_factor;

    train_scale(image);

    if (_roi.x >= image.cols - 1) _roi.x = image.cols - 1;
    if (_roi.y >= image.rows - 1) _roi.y = image.rows - 1;
    if (_roi.x + _roi.width <= 0) _roi.x = -_roi.width + 2;
    if (_roi.y + _roi.height <= 0) _roi.y = -_roi.height + 2;


    assert(_roi.width >= 0 && _roi.height >= 0);
    cv::Mat x = getFeatures(image, 0);
    train(x, interp_factor);


    return _roi;
}

// Detect the new scaling rate
cv::Point2i KCFTracker::detect_scale(cv::Mat image)
{
  cv::Mat xsf = KCFTracker::get_scale_sample(image);

  // Compute AZ in the paper
  cv::Mat add_temp;
  cv::reduce(FFTTools::complexMultiplication(sf_num, xsf), add_temp, 0, CV_REDUCE_SUM);

  // compute the final y
  cv::Mat scale_response;
  cv::idft(FFTTools::complexDivisionReal(add_temp, (sf_den + scale_lambda)), scale_response, cv::DFT_REAL_OUTPUT);

  // Get the max point as the final scaling rate
  cv::Point2i pi;
  double pv;
  cv::minMaxLoc(scale_response, NULL, &pv, NULL, &pi);

  return pi;
}


// Detect object in the current frame.
cv::Point2f KCFTracker::detect(cv::Mat z, cv::Mat x, float &peak_value)
{
    using namespace FFTTools;

    cv::Mat k = gaussianCorrelation(x, z);
    cv::Mat res = (real(fftd(complexMultiplication(_alphaf, fftd(k)), true)));

    //minMaxLoc only accepts doubles for the peak, and integer points for the coordinates
    cv::Point2i pi;
    double pv;
    cv::minMaxLoc(res, NULL, &pv, NULL, &pi);
    peak_value = (float) pv;

    //subpixel peak estimation, coordinates will be non-integer
    cv::Point2f p((float)pi.x, (float)pi.y);

    if (pi.x > 0 && pi.x < res.cols-1) {
        p.x += subPixelPeak(res.at<float>(pi.y, pi.x-1), peak_value, res.at<float>(pi.y, pi.x+1));
    }

    if (pi.y > 0 && pi.y < res.rows-1) {
        p.y += subPixelPeak(res.at<float>(pi.y-1, pi.x), peak_value, res.at<float>(pi.y+1, pi.x));
    }

    p.x -= (res.cols) / 2;
    p.y -= (res.rows) / 2;

    return p;
}

// train tracker with a single image
void KCFTracker::train(cv::Mat x, float train_interp_factor)
{
    using namespace FFTTools;

    cv::Mat k = gaussianCorrelation(x, x);
    cv::Mat alphaf = complexDivision(_prob, (fftd(k) + lambda));

    _tmpl = (1 - train_interp_factor) * _tmpl + (train_interp_factor) * x;
    _alphaf = (1 - train_interp_factor) * _alphaf + (train_interp_factor) * alphaf;


    /*cv::Mat kf = fftd(gaussianCorrelation(x, x));
    cv::Mat num = complexMultiplication(kf, _prob);
    cv::Mat den = complexMultiplication(kf, kf + lambda);

    _tmpl = (1 - train_interp_factor) * _tmpl + (train_interp_factor) * x;
    _num = (1 - train_interp_factor) * _num + (train_interp_factor) * num;
    _den = (1 - train_interp_factor) * _den + (train_interp_factor) * den;

    _alphaf = complexDivision(_num, _den);*/

}

// Evaluates a Gaussian kernel with bandwidth SIGMA for all relative shifts between input images X and Y, which must both be MxN. They must    also be periodic (ie., pre-processed with a cosine window).
cv::Mat KCFTracker::gaussianCorrelation(cv::Mat x1, cv::Mat x2)
{
    using namespace FFTTools;
    cv::Mat c = cv::Mat( cv::Size(size_patch[1], size_patch[0]), CV_32F, cv::Scalar(0) );
    // HOG features
    if (_hogfeatures) {
        cv::Mat caux;
        cv::Mat x1aux;
        cv::Mat x2aux;
        for (int i = 0; i < size_patch[2]; i++) {
            x1aux = x1.row(i);   // Procedure do deal with cv::Mat multichannel bug
            x1aux = x1aux.reshape(1, size_patch[0]);
            x2aux = x2.row(i).reshape(1, size_patch[0]);
            cv::mulSpectrums(fftd(x1aux), fftd(x2aux), caux, 0, true);
            caux = fftd(caux, true);
            rearrange(caux);
            caux.convertTo(caux,CV_32F);
            c = c + real(caux);
        }
    }
    // Gray features
    else {
        cv::mulSpectrums(fftd(x1), fftd(x2), c, 0, true);
        c = fftd(c, true);
        rearrange(c);
        c = real(c);
    }
    cv::Mat d;
    cv::max(( (cv::sum(x1.mul(x1))[0] + cv::sum(x2.mul(x2))[0])- 2. * c) / (size_patch[0]*size_patch[1]*size_patch[2]) , 0, d);

    cv::Mat k;
    cv::exp((-d / (sigma * sigma)), k);
    return k;
}

// Create Gaussian Peak. Function called only in the first frame.
cv::Mat KCFTracker::createGaussianPeak(int sizey, int sizex)
{
    cv::Mat_<float> res(sizey, sizex);

    int syh = (sizey) / 2;
    int sxh = (sizex) / 2;

    float output_sigma = std::sqrt((float) sizex * sizey) / padding * output_sigma_factor;
    float mult = -0.5 / (output_sigma * output_sigma);

    for (int i = 0; i < sizey; i++)
        for (int j = 0; j < sizex; j++)
        {
            int ih = i - syh;
            int jh = j - sxh;
            res(i, j) = std::exp(mult * (float) (ih * ih + jh * jh));
        }
    return FFTTools::fftd(res);
}

// Obtain sub-window from image, with replication-padding and extract features
cv::Mat KCFTracker::getFeatures(const cv::Mat & image, bool inithann, float scale_adjust)
{
    cv::Rect extracted_roi;

    float cx = _roi.x + _roi.width / 2;
    float cy = _roi.y + _roi.height / 2;

    if (inithann) {
        int padded_w = _roi.width * padding;
        int padded_h = _roi.height * padding;

        if (template_size > 1) {  // Fit largest dimension to the given template size
            if (padded_w >= padded_h)  //fit to width
                _scale = padded_w / (float) template_size;
            else
                _scale = padded_h / (float) template_size;

            _tmpl_sz.width = padded_w / _scale;
            _tmpl_sz.height = padded_h / _scale;
        }
        else {  //No template size given, use ROI size
            _tmpl_sz.width = padded_w;
            _tmpl_sz.height = padded_h;
            _scale = 1;
            // original code from paper:
            /*if (sqrt(padded_w * padded_h) >= 100) {   //Normal size
                _tmpl_sz.width = padded_w;
                _tmpl_sz.height = padded_h;
                _scale = 1;
            }
            else {   //ROI is too big, track at half size
                _tmpl_sz.width = padded_w / 2;
                _tmpl_sz.height = padded_h / 2;
                _scale = 2;
            }*/
        }

        if (_hogfeatures) {
            // Round to cell size and also make it even
            _tmpl_sz.width = ( ( (int)(_tmpl_sz.width / (2 * cell_size)) ) * 2 * cell_size ) + cell_size*2;
            _tmpl_sz.height = ( ( (int)(_tmpl_sz.height / (2 * cell_size)) ) * 2 * cell_size ) + cell_size*2;
        }
        else {  //Make number of pixels even (helps with some logic involving half-dimensions)
            _tmpl_sz.width = (_tmpl_sz.width / 2) * 2;
            _tmpl_sz.height = (_tmpl_sz.height / 2) * 2;
        }
    }

    extracted_roi.width = scale_adjust * _scale * _tmpl_sz.width * currentScaleFactor;
    extracted_roi.height = scale_adjust * _scale * _tmpl_sz.height * currentScaleFactor;

    // center roi with new size
    extracted_roi.x = cx - extracted_roi.width / 2;
    extracted_roi.y = cy - extracted_roi.height / 2;

    cv::Mat FeaturesMap;
    cv::Mat z = RectTools::subwindow(image, extracted_roi, cv::BORDER_REPLICATE);

    if (z.cols != _tmpl_sz.width || z.rows != _tmpl_sz.height) {
        cv::resize(z, z, _tmpl_sz);
    }

    imshow("z", z);

    // HOG features
    if (_hogfeatures) {
        IplImage z_ipl = z;
        CvLSVMFeatureMapCaskade *map;
        CvLSVMFeatureMapCaskade *map1;
        getFeatureMaps(&z_ipl, cell_size, &map);
        calcFeatureMaps(&z_ipl, cell_size, &map1);
        int diff = compare_featuremap(map, map1);
        if (diff < 0)
            printf("feature is diff!\n");
        else
        {
            printf("feature is same!\n");
        }
        
        normalizeAndTruncate(map,0.2f);
        PCAFeatureMaps(map);
        size_patch[0] = map->sizeY;
        size_patch[1] = map->sizeX;
        size_patch[2] = map->numFeatures;

        FeaturesMap = cv::Mat(cv::Size(map->numFeatures,map->sizeX*map->sizeY), CV_32F, map->map);  // Procedure do deal with cv::Mat multichannel bug
        FeaturesMap = FeaturesMap.t();
        
        freeFeatureMapObject(&map);

        // Lab features
        if (_labfeatures) {
            cv::Mat imgLab;
            cvtColor(z, imgLab, CV_BGR2Lab);
            unsigned char *input = (unsigned char*)(imgLab.data);

            // Sparse output vector
            cv::Mat outputLab = cv::Mat(_labCentroids.rows, size_patch[0]*size_patch[1], CV_32F, float(0));

            int cntCell = 0;
            // Iterate through each cell
            for (int cY = cell_size; cY < z.rows-cell_size; cY+=cell_size){
                for (int cX = cell_size; cX < z.cols-cell_size; cX+=cell_size){
                    // Iterate through each pixel of cell (cX,cY)
                    for(int y = cY; y < cY+cell_size; ++y){
                        for(int x = cX; x < cX+cell_size; ++x){
                            // Lab components for each pixel
                            float l = (float)input[(z.cols * y + x) * 3];
                            float a = (float)input[(z.cols * y + x) * 3 + 1];
                            float b = (float)input[(z.cols * y + x) * 3 + 2];

                            // Iterate trough each centroid
                            float minDist = FLT_MAX;
                            int minIdx = 0;
                            float *inputCentroid = (float*)(_labCentroids.data);
                            for(int k = 0; k < _labCentroids.rows; ++k){
                                float dist = ( (l - inputCentroid[3*k]) * (l - inputCentroid[3*k]) )
                                           + ( (a - inputCentroid[3*k+1]) * (a - inputCentroid[3*k+1]) )
                                           + ( (b - inputCentroid[3*k+2]) * (b - inputCentroid[3*k+2]) );
                                if(dist < minDist){
                                    minDist = dist;
                                    minIdx = k;
                                }
                            }
                            // Store result at output
                            outputLab.at<float>(minIdx, cntCell) += 1.0 / cell_sizeQ;
                            //((float*) outputLab.data)[minIdx * (size_patch[0]*size_patch[1]) + cntCell] += 1.0 / cell_sizeQ;
                        }
                    }
                    cntCell++;
                }
            }
            // Update size_patch[2] and add features to FeaturesMap
            size_patch[2] += _labCentroids.rows;
            FeaturesMap.push_back(outputLab);
        }
    }
    else {
        FeaturesMap = RectTools::getGrayImage(z);
        FeaturesMap -= (float) 0.5; // In Paper;
        size_patch[0] = z.rows;
        size_patch[1] = z.cols;
        size_patch[2] = 1;
    }

    if (inithann) {
        createHanningMats();
    }
    FeaturesMap = hann.mul(FeaturesMap);
    return FeaturesMap;
}

// Initialize Hanning window. Function called only in the first frame.
void KCFTracker::createHanningMats()
{
    cv::Mat hann1t = cv::Mat(cv::Size(size_patch[1],1), CV_32F, cv::Scalar(0));
    cv::Mat hann2t = cv::Mat(cv::Size(1,size_patch[0]), CV_32F, cv::Scalar(0));

    for (int i = 0; i < hann1t.cols; i++)
        hann1t.at<float > (0, i) = 0.5 * (1 - std::cos(2 * 3.14159265358979323846 * i / (hann1t.cols - 1)));
    for (int i = 0; i < hann2t.rows; i++)
        hann2t.at<float > (i, 0) = 0.5 * (1 - std::cos(2 * 3.14159265358979323846 * i / (hann2t.rows - 1)));

    cv::Mat hann2d = hann2t * hann1t;
    // HOG features
    if (_hogfeatures) {
        cv::Mat hann1d = hann2d.reshape(1,1); // Procedure do deal with cv::Mat multichannel bug

        hann = cv::Mat(cv::Size(size_patch[0]*size_patch[1], size_patch[2]), CV_32F, cv::Scalar(0));
        for (int i = 0; i < size_patch[2]; i++) {
            for (int j = 0; j<size_patch[0]*size_patch[1]; j++) {
                hann.at<float>(i,j) = hann1d.at<float>(0,j);
            }
        }
    }
    // Gray features
    else {
        hann = hann2d;
    }
}

// Calculate sub-pixel peak for one dimension
float KCFTracker::subPixelPeak(float left, float center, float right)
{
    float divisor = 2 * center - right - left;

    if (divisor == 0)
        return 0;

    return 0.5 * (right - left) / divisor;
}

// Initialization for scales
void KCFTracker::dsstInit(const cv::Rect &roi, cv::Mat image)
{
  // The initial size for adjusting
  base_width = roi.width;
  base_height = roi.height;

  // Guassian peak for scales (after fft)
  ysf = computeYsf();
  s_hann = createHanningMatsForScale();

  // Get all scale changing rate
  scaleFactors = new float[n_scales];
  float ceilS = std::ceil(n_scales / 2.0f);
  for(int i = 0 ; i < n_scales; i++)
  {
    scaleFactors[i] = std::pow(scale_step, ceilS - i - 1);
  }

  // Get the scaling rate for compressing to the model size
  float scale_model_factor = 1;
  if(base_width * base_height > scale_max_area)
  {
    scale_model_factor = std::sqrt(scale_max_area / (float)(base_width * base_height));
  }
  scale_model_width = (int)(base_width * scale_model_factor);
  scale_model_height = (int)(base_height * scale_model_factor);

  // Compute min and max scaling rate
  min_scale_factor = std::pow(scale_step,
    std::ceil(std::log((std::fmax(5 / (float) base_width, 5 / (float) base_height) * (1 + scale_padding))) / 0.0086));
  max_scale_factor = std::pow(scale_step,
    std::floor(std::log(std::fmin(image.rows / (float) base_height, image.cols / (float) base_width)) / 0.0086));

  train_scale(image, true);

}

// Train method for scaling
void KCFTracker::train_scale(cv::Mat image, bool ini)
{
  cv::Mat xsf = get_scale_sample(image);

  // Adjust ysf to the same size as xsf in the first time
  if(ini)
  {
    int totalSize = xsf.rows;
    ysf = cv::repeat(ysf, totalSize, 1);
  }

  // Get new GF in the paper (delta A)
  cv::Mat new_sf_num;
  cv::mulSpectrums(ysf, xsf, new_sf_num, 0, true);

  // Get Sigma{FF} in the paper (delta B)
  cv::Mat new_sf_den;
  cv::mulSpectrums(xsf, xsf, new_sf_den, 0, true);
  cv::reduce(FFTTools::real(new_sf_den), new_sf_den, 0, CV_REDUCE_SUM);

  if(ini)
  {
    sf_den = new_sf_den;
    sf_num = new_sf_num;
  }else
  {
    // Get new A and new B
    cv::addWeighted(sf_den, (1 - scale_lr), new_sf_den, scale_lr, 0, sf_den);
    cv::addWeighted(sf_num, (1 - scale_lr), new_sf_num, scale_lr, 0, sf_num);
  }

  update_roi();

}

// Update the ROI size after training
void KCFTracker::update_roi()
{
  // Compute new center
  float cx = _roi.x + _roi.width / 2.0f;
  float cy = _roi.y + _roi.height / 2.0f;

  // printf("%f\n", currentScaleFactor);

  // Recompute the ROI left-upper point and size
  _roi.width = base_width * currentScaleFactor;
  _roi.height = base_height * currentScaleFactor;

  _roi.x = cx - _roi.width / 2.0f;
  _roi.y = cy - _roi.height / 2.0f;

}

// Compute the F^l in the paper
cv::Mat KCFTracker::get_scale_sample(const cv::Mat & image)
{
  CvLSVMFeatureMapCaskade *map[n_scales]; // temporarily store FHOG result
  cv::Mat xsf; // output
  int totalSize; // # of features
  cv::Size ssize;


  for(int i = 0; i < n_scales; i++)
  {
    map[i] = NULL;
  }

  for(int i = 0; i < n_scales; i++)
  {
    // Size of subwindow waiting to be detect
    float patch_width = base_width * scaleFactors[i] * currentScaleFactor;
    float patch_height = base_height * scaleFactors[i] * currentScaleFactor;

    float cx = _roi.x + _roi.width / 2.0f;
    float cy = _roi.y + _roi.height / 2.0f;

    // Get the subwindow
    cv::Mat im_patch = RectTools::extractImage(image, cx, cy, patch_width, patch_height);
    cv::Mat im_patch_resized;

    ssize = im_patch.size();

    //printf("im_patch_width=%d\n",ssize.width);
    //printf("im_patch_height=%d\n",ssize.height);
    
    if(ssize.width <= 0 || ssize.height <= 0)
    {
       continue;     
    }

    // Scaling the subwindow
    if(scale_model_width > im_patch.cols)
      resize(im_patch, im_patch_resized, cv::Size(scale_model_width, scale_model_height), 0, 0, 1);
    else
      resize(im_patch, im_patch_resized, cv::Size(scale_model_width, scale_model_height), 0, 0, 3);

    // Compute the FHOG features for the subwindow
    IplImage im_ipl = im_patch_resized;
    getFeatureMaps(&im_ipl, cell_size, &map[i]);
    normalizeAndTruncate(map[i], 0.2f);
    PCAFeatureMaps(map[i]);

    if(i == 0)
    {
      totalSize = map[i]->numFeatures*map[i]->sizeX*map[i]->sizeY;
      xsf = cv::Mat(cv::Size(n_scales,totalSize), CV_32F, float(0));
    }

    // Multiply the FHOG results by hanning window and copy to the output
    cv::Mat FeaturesMap = cv::Mat(cv::Size(1, totalSize), CV_32F, map[i]->map);
    float mul = s_hann.at<float > (0, i);
    FeaturesMap = mul * FeaturesMap;
    FeaturesMap.copyTo(xsf.col(i));

  }

  // Free the temp variables
  for(int i = 0; i < n_scales; i++)
      freeFeatureMapObject(&map[i]);

  // Do fft to the FHOG features row by row
  xsf = FFTTools::fftd(xsf, 0, 1);

  return xsf;
}

// Compute the FFT Guassian Peak for scaling
cv::Mat KCFTracker::computeYsf()
{
    float scale_sigma2 = n_scales / std::sqrt(n_scales) * scale_sigma_factor;
    scale_sigma2 = scale_sigma2 * scale_sigma2;
    cv::Mat res(cv::Size(n_scales, 1), CV_32F, float(0));
    float ceilS = std::ceil(n_scales / 2.0f);

    for(int i = 0; i < n_scales; i++)
    {
      res.at<float>(0,i) = std::exp(- 0.5 * std::pow(i + 1- ceilS, 2) / scale_sigma2);
    }

    return FFTTools::fftd(res);

}

// Compute the hanning window for scaling
cv::Mat KCFTracker::createHanningMatsForScale()
{
  cv::Mat hann_s = cv::Mat(cv::Size(n_scales, 1), CV_32F, cv::Scalar(0));
  for (int i = 0; i < hann_s.cols; i++)
      hann_s.at<float > (0, i) = 0.5 * (1 - std::cos(2 * 3.14159265358979323846 * i / (hann_s.cols - 1)));

  return hann_s;
}
