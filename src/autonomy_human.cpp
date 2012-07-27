#include <ros/ros.h>
#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/RegionOfInterest.h>
#include <std_msgs/Header.h>

#include "opencv2/objdetect/objdetect.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/video/tracking.hpp"

#include <boost/thread.hpp>

#include "autonomy_human/human.h"

using namespace cv;
using namespace std;

class CHumanTracker
{
public:
	enum _trackingState{
		STATE_LOST = 0,
		STATE_DETECT = 1,
		STATE_TRACK = 2,
		STATE_REJECT = 3	
	};
	
	enum _gestureRegionIds {
		REG_TOPLEFT = 0,
		REG_TOPRIGHT = 1,
		REG_BOTLEFT = 2,
		REG_BOTRIGHT =3
	};
	
private:
	int iWidth;
	int iHeight;
	//ros::NodeHandle node;
	Mat rawFrame;	
	Rect searchROI;
	Rect flowROI;
	Mat rawFramGray;
	Mat prevRawFrameGray;
	
	// Face Tracker
	KalmanFilter* KFTracker; 
	KalmanFilter* MLSearch; // Maximum Likelihood Search for multiple faces
	Mat state; // x,y,xdot,ydot,w,h
	Mat measurement;
	
	// State Machine	
	int stateCounter;
	int initialScoreMin;
	int minDetectFrames;
	int minRejectFrames;
	float maxRejectCov;
	Size minFaceSize;
	Size maxFaceSize;

	// Histograms (Skin)
	MatND faceHist;
	int hbins;
	int sbins;
	Mat skin;
	Mat skinPrior;
	Mat flow;
	Mat flowMag;
	
	// Optical Flow
	int minFlow;
	
	
	//Time measurements
	ros::Time tStart;
	ros::Time tFaceStart;
	ros::Time tSkinStart;
	ros::Time tFlowStart;
	ros::Time tEnd;
	
	string strStates[4];
	
	/*
	 * Bit 0: Original Image
	 * Bit 1: Face Image
	 * Bit 2: Skin Image
     * Bit 3: Histogram
	 * Bit 4: Optical Flow
	 */
	unsigned short int debugLevel;
	bool skinEnabled;
	CvHaarClassifierCascade* cascade; // C API is needed for score
	
	void copyKalman(KalmanFilter* src, KalmanFilter* dest);
	void resetKalmanFilter();
	void initMats();
	void resetMats();
	void generateRegionHistogram(Mat& region, MatND &hist, bool vis = true);
	void histFilter(Mat& region, Mat& post, Mat& prior, MatND& hist,  bool vis = true);
	void detectAndTrackFace();
    void trackSkin();
	void calcOpticalFlow();
	void draw();
	void safeRect(Rect &r, Rect &boundry);
	void safeRectToImage(Rect &r);
	
    // Threading
    boost::thread displayThread;
	
public:
	CHumanTracker(string &cascadeFile, int _initialScoreMin, int _initialDetectFrames, int _initialRejectFrames, int _minFlow, bool _skinEnabled, unsigned short int _debugLevel);
	~CHumanTracker();
	void visionCallback(const sensor_msgs::ImageConstPtr& frame);
	void reset();
	float calcMedian(Mat &m, int nbins, float minVal, float maxVal, InputArray mask = noArray());
	
    bool isInited;
    Mat debugFrame;
	Mat skinFrame;
	Mat opticalFrame;
	
	vector<Rect> faces;
	int faceScore;
	Rect beleif;
	_trackingState trackingState;
	bool isFaceInCurrentFrame;
	
	bool shouldPublish;
	
	/*
	 * Drone Point of View, Face in center
	 * 0 : Top left
	 * 1 : Top right
	 * 2 : Bottom left
	 * 3 : Bottom right
 	 */
	Rect gestureRegion[4];
	float flowScoreInRegion[4];
};

CHumanTracker::CHumanTracker(string &cascadeFile, int _initialScoreMin, int _initialDetectFrames, int _initialRejectFrames, int _minFlow, bool _skinEnabled, unsigned short int _debugLevel)
{
	isInited = false;
	shouldPublish = false;
	
	stateCounter = 0;
	
	skinEnabled = _skinEnabled;
	debugLevel = _debugLevel;	
	initialScoreMin = _initialScoreMin;
	minDetectFrames = _initialDetectFrames;
	minRejectFrames = _initialRejectFrames;
	
	maxRejectCov = 6.0;
	
	hbins = 15;
	sbins = 16;
	
	minFlow = _minFlow;
		
	strStates[0] = "NOFACE";
	strStates[1] = "DETECT";
	strStates[2] = "TRACKG";
	strStates[3] = "REJECT";
	
	minFaceSize = Size(20, 25);
	maxFaceSize = Size(100,100);
	
	cascade = (CvHaarClassifierCascade*) cvLoad(cascadeFile.c_str(), 0, 0, 0);
	if (cascade == NULL)
	{
		ROS_ERROR("Problem loading cascade file %s", cascadeFile.c_str());
	}
	
	KFTracker = new KalmanFilter(6, 4, 0);
	MLSearch = new KalmanFilter(6, 4, 0);
    isFaceInCurrentFrame = false;
}

CHumanTracker::~CHumanTracker()
{
	delete KFTracker;
	delete MLSearch;
}

void CHumanTracker::copyKalman(KalmanFilter* src, KalmanFilter* dest)
{
	dest->measurementNoiseCov = src->measurementNoiseCov.clone();
	dest->controlMatrix = src->controlMatrix.clone();
	dest->errorCovPost = src->errorCovPost.clone();
	dest->errorCovPre = src->errorCovPre.clone();
	dest->gain = src->gain.clone();
	dest->measurementMatrix = src->measurementMatrix.clone();
	dest->processNoiseCov = src->processNoiseCov.clone();
	dest->statePost = src->statePost.clone();
	dest->statePre = src->statePre.clone();
	dest->transitionMatrix = src->transitionMatrix.clone();
}

void CHumanTracker::reset()
{
	trackingState = STATE_LOST;
	searchROI = Rect(Point(0,0), Point(iWidth,iHeight));
	
	// x,y,xdot,ydot,w,h
	state = Mat::zeros(6, 1, CV_32F); 
	
	// x,y,w,h
	measurement = Mat::zeros(4, 1, CV_32F);
	
	isFaceInCurrentFrame = false;
	
	for (int i = 0; i < 4; i++)
		flowScoreInRegion[i] = 0.0;
	
	initMats();
	resetMats();
	resetKalmanFilter();
}

void CHumanTracker::initMats()
{
	skin = Mat::zeros(iHeight, iWidth, CV_32F);
	skinPrior = Mat::zeros(iHeight, iWidth, CV_32F);
	faceHist = Mat::zeros(hbins, sbins, CV_32F);
    skinFrame = Mat::zeros(iHeight, iWidth, CV_8UC1);
	opticalFrame = Mat::zeros(iHeight, iWidth, CV_8UC3);
	flow = Mat::zeros(iHeight, iWidth, CV_32FC2);
	flowMag = Mat::zeros(iHeight, iWidth, CV_32F);
}

void CHumanTracker::resetMats()
{
	skinPrior = Scalar::all( 1.0 / (iWidth * iHeight)); 
	skin = Scalar::all(1.0 / (skin.rows * skin.cols));
	faceHist = Scalar::all(1.0 / (faceHist.rows * faceHist.cols) );
}
void CHumanTracker::resetKalmanFilter()
{	
	measurement.setTo(Scalar(0));
	KFTracker->transitionMatrix = * (Mat_<float>(6,6) 
			<< 
			1,0,1,0,0,0,
			0,1,0,1,0,0, 
			0,0,1,0,0,0,
			0,0,0,1,0,0,
			0,0,0,0,1,0,
			0,0,0,0,0,1
			);

	KFTracker->measurementMatrix = *(Mat_<float>(4,6) 
			<< 
			1,0,0,0,0,0,
			0,1,0,0,0,0,
			0,0,0,0,1,0,
			0,0,0,0,0,1
			);

	KFTracker->statePre = *(Mat_<float>(6,1) << 0,0,0,0,0,0);
	KFTracker->statePost = *(Mat_<float>(6,1) << 0,0,0,0,0,0);
	
	setIdentity(KFTracker->errorCovPre, Scalar_<float>::all(1e2));
	setIdentity(KFTracker->errorCovPost, Scalar_<float>::all(1e2));
	setIdentity(KFTracker->processNoiseCov, Scalar_<float>::all(0.05));
	setIdentity(KFTracker->measurementNoiseCov, Scalar_<float>::all(1.0));

	copyKalman(KFTracker, MLSearch);
			
	
}

float CHumanTracker::calcMedian(Mat &m, int nbins, float minVal, float maxVal, InputArray mask)
{
	MatND hist;
	int hsize[1];
	hsize[0] = nbins;
	float range[2];
	range[0] = minVal; 
	range[1] = maxVal;
	const float *ranges[] = {range};	
	int chnls[] = {0};
		
	calcHist(&m, 1, chnls, mask, hist, 1, hsize, ranges);
	
	float step = (maxVal - minVal) / nbins;
	long int sumHist = sum(hist)[0];
	long int sum = 0;
	float median;
	for (int i = 0; i < nbins; i++)
	{
		sum += hist.at<float>(i, 0);
		median = minVal + (i * step) + (step / 2.0);
		if (sum >= (sumHist / 2.0))
		{			
			break;
		}
	}
	return median;
}

void CHumanTracker::safeRect(Rect& r, Rect& boundry)
{
	r.x = max<int>(r.x, boundry.x);
	r.y = max<int>(r.y, boundry.y);

	Point2d p2;
	p2.x = min<int>(r.br().x, boundry.br().x);
	p2.y = min<int>(r.br().y, boundry.br().y);
	
	r.width = p2.x - r.x;
	r.height = p2.y - r.y;
}

void CHumanTracker::safeRectToImage(Rect& r)
{
	Rect safety(0, 0, iWidth, iHeight);
	safeRect(r, safety);
}

void CHumanTracker::generateRegionHistogram(Mat& region, MatND &hist, bool vis)
{
	MatND prior = hist.clone();
	
	Mat hsv;
	cvtColor(region, hsv, CV_BGR2HSV);
	
	Mat mask = Mat::zeros(region.rows, region.cols, CV_8UC1);
	for (int r = 0; r < region.rows; r++)
	{
		for (int c = 0; c < region.cols; c++)
		{
			unsigned int v = hsv.at<Vec3b>(r,c)[2];
			// TODO: Make me parameters
			if (( v > 50) && (v < 150))
			{
				mask.at<uchar>(r,c) = 1;
			}
		}
	}
	
//	namedWindow( "Face Mask", 1 );
//	imshow( "Face Mask", mask * 255 );
	
	int histSize[] = {hbins, sbins};
	float hranges[] = {0, 180};
	float sranges[] = {0, 256};
	const float* ranges[] = { hranges, sranges};	
	int channels[] = {0, 1};
	calcHist(&hsv, 1, channels, mask, hist, 2, histSize, ranges, true, false);
	
	for( int h = 0; h < hbins; h++ )
	{
		for( int s = 0; s < sbins; s++ )
		{		
			hist.at<float>(h,s) = (0.99 * prior.at<float>(h,s)) + (0.01 * hist.at<float>(h,s));
		}
	}
	
	// We should make it a probability dist.
	normalize(hist, hist, 1.0, 0.0, NORM_L1);
	
	if (vis)
	{
		// For vis
		Mat visHist;
		normalize(hist, visHist, 255.0, 0.00, NORM_MINMAX);

		int scale = 10;
		Mat histImg = Mat::zeros(sbins * scale, hbins* scale, CV_8UC3);
		for( int h = 0; h < hbins; h++ )
		{
			for( int s = 0; s < sbins; s++ )
			{
				float binVal = visHist.at<float>(h, s);
				int intensity = cvRound(binVal);///maxVal);
				rectangle( histImg, Point(h*scale, s*scale),
							Point( (h+1)*scale - 1, (s+1)*scale - 1),
							Scalar::all(intensity),
							CV_FILLED );
			}
		}

        if ((debugLevel & 0x08) == 0x08)
        {
            namedWindow( "H-S Histogram", 1 );
            imshow( "H-S Histogram", histImg );
            waitKey(1);
        }
	}
}

void CHumanTracker::histFilter(Mat& region, Mat& post, Mat& prior, MatND& hist,  bool vis)
{
	int _w = region.cols;
	int _h = region.rows;
	Mat image;
	cvtColor(region, image, CV_BGR2HSV);
	for (int r = 0; r < _h; r++ )
	{
		for (int c = 0; c < _w; c++)
		{
			int hue_bin = cvFloor(image.at<cv::Vec3b>(r,c)[0] / (180.0 / (float) hbins) );
			int sat_bin = cvFloor(image.at<cv::Vec3b>(r,c)[1] / (256.0 / (float) sbins) );
			
			float z = hist.at<float>(hue_bin, sat_bin);
			post.at<float>(r,c) = (0.7 * z) + (0.3 * prior.at<float>(r, c));
			//post.at<float>(r,c) = z * prior.at<float>(r,c);
		}
	}
	
	if (vis)
	{
		// For vis;
		Mat visPost;
		normalize(post, visPost, 1.0, 0.0, NORM_MINMAX);
		namedWindow( "Skin", 1 );
		imshow( "Skin", visPost);
	}
	//prior = post.clone();
}

void CHumanTracker::detectAndTrackFace()
{
	static ros::Time probe;
	
	// Do ROI 	
	debugFrame = rawFrame.clone();
	Mat img =  this->rawFrame(searchROI);
	
	faces.clear();
	ostringstream txtstr;
    const static Scalar colors[] =  { CV_RGB(0,0,255),
        CV_RGB(0,128,255),
        CV_RGB(0,255,255),
        CV_RGB(0,255,0),
        CV_RGB(255,128,0),
        CV_RGB(255,255,0),
        CV_RGB(255,0,0),
        CV_RGB(255,0,255)} ;
    Mat gray;
    Mat frame( cvRound(img.rows), cvRound(img.cols), CV_8UC1 );
    cvtColor( img, gray, CV_BGR2GRAY );
    resize( gray, frame, frame.size(), 0, 0, INTER_LINEAR );
    //equalizeHist( frame, frame );

	// This if for internal usage
	ros::Time _n = ros::Time::now();
	double dt = (_n - probe).toSec();
	probe = _n;
	
	// We need C API to get score
	MemStorage storage(cvCreateMemStorage(0));
	CvMat _image = frame;
	CvSeq* _objects = cvHaarDetectObjects(&_image, cascade, storage, 
			1.2, initialScoreMin, CV_HAAR_DO_CANNY_PRUNING|CV_HAAR_SCALE_IMAGE, minFaceSize, maxFaceSize);
	
	vector<CvAvgComp> vecAvgComp;
	Seq<CvAvgComp>(_objects).copyTo(vecAvgComp);
	// End of using C API
	
	isFaceInCurrentFrame = (vecAvgComp.size() > 0);
	
	if (trackingState == STATE_LOST)
	{
		if (isFaceInCurrentFrame)
		{
			stateCounter++;
			trackingState = STATE_DETECT;
		}
	}
	
	if (trackingState == STATE_DETECT)
	{
		if (isFaceInCurrentFrame)
		{
			stateCounter++;			
		}
		else
		{
			stateCounter = 0;
			trackingState = STATE_LOST;
		}
		
		if (stateCounter > minDetectFrames)
		{
			stateCounter = 0;
			trackingState = STATE_TRACK;
		}

	}
	
	if (trackingState == STATE_TRACK)
	{
		if (!isFaceInCurrentFrame)
		{
			trackingState = STATE_REJECT;
		}
	}
	
	if (trackingState == STATE_REJECT)
	{
		float covNorm = sqrt(
						pow(KFTracker->errorCovPost.at<float>(0,0), 2) +
						pow(KFTracker->errorCovPost.at<float>(1,1), 2) 
						);
		
		if (!isFaceInCurrentFrame)
		{
			stateCounter++;
		}
		else
		{
			stateCounter = 0;
			trackingState = STATE_TRACK;
		}
		
		if ((stateCounter > minRejectFrames) && (covNorm > maxRejectCov))
		{
			trackingState = STATE_LOST;
			stateCounter = 0;
			resetKalmanFilter();	
            reset();
		}
	}
		
	if ((trackingState == STATE_TRACK) || (trackingState == STATE_REJECT))
	{
		bool updateFaceHist = false;
		// This is important:

		KFTracker->transitionMatrix.at<float>(0,2) = dt;
		KFTracker->transitionMatrix.at<float>(1,3) = dt;

		Mat pred = KFTracker->predict();      

		if (isFaceInCurrentFrame)
		{
			CvAvgComp MLFace;
			float minCovNorm = 1e24;
			int i = 0;
			for( vector<CvAvgComp>::const_iterator rr = vecAvgComp.begin(); rr != vecAvgComp.end(); rr++, i++ )
			{
				copyKalman(KFTracker, MLSearch);  
				CvRect r = rr->rect;
				r.x += searchROI.x;
				r.y += searchROI.y;
				double nr = rr->neighbors;
				Point center;
				Scalar color = colors[i%8];

				float normFaceScore = 1.0 - (nr / 40.0);
				if (normFaceScore > 1.0) normFaceScore = 1.0;
				if (normFaceScore < 0.0) normFaceScore = 0.0;
				setIdentity(MLSearch->measurementNoiseCov, Scalar_<float>::all(normFaceScore));

				center.x = cvRound(r.x + r.width*0.5);
				center.y = cvRound(r.y + r.height*0.5);
				
				measurement.at<float>(0) = r.x;
				measurement.at<float>(1) = r.y;
				measurement.at<float>(2) = r.width;
				measurement.at<float>(3) = r.height;
				
				MLSearch->correct(measurement);
				
				float covNorm = sqrt(
					pow(MLSearch->errorCovPost.at<float>(0,0), 2) +
					pow(MLSearch->errorCovPost.at<float>(1,1), 2) 
				);

				if (covNorm < minCovNorm) 
				{
					minCovNorm = covNorm;
					MLFace = *rr;
				}
				
                if ((debugLevel & 0x02) == 0x02)
                {
                    rectangle(debugFrame, center - Point(r.width*0.5, r.height*0.5), center + Point(r.width*0.5, r.height * 0.5), color);

                    txtstr.str("");
                    txtstr << "   N:" << rr->neighbors << " S:" << r.width << "x" << r.height;

                    putText(debugFrame, txtstr.str(), center, FONT_HERSHEY_PLAIN, 1, color);
                }
			}

			// TODO: I'll fix this shit
			Rect r = MLFace.rect;			
			r.x += searchROI.x;
			r.y += searchROI.y;
			faces.push_back(r);
			double nr = MLFace.neighbors;
			faceScore = nr;
			float normFaceScore = 1.0 - (nr / 40.0);
			if (normFaceScore > 1.0) normFaceScore = 1.0;
			if (normFaceScore < 0.0) normFaceScore = 0.0;
			setIdentity(KFTracker->measurementNoiseCov, Scalar_<float>::all(normFaceScore));
			measurement.at<float>(0) = r.x;
			measurement.at<float>(1) = r.y;
			measurement.at<float>(2) = r.width;
			measurement.at<float>(3) = r.height;
			KFTracker->correct(measurement);
            
            // We see a face
			updateFaceHist = true;
		}
		else
		{
			KFTracker->statePost = KFTracker->statePre;
			KFTracker->errorCovPost = KFTracker->errorCovPre;
		}
		
		beleif.x = max<int>(KFTracker->statePost.at<float>(0), 0);
		beleif.y = max<int>(KFTracker->statePost.at<float>(1), 0);
		beleif.width = min<int>(KFTracker->statePost.at<float>(4), iWidth - beleif.x);
		beleif.height = min<int>(KFTracker->statePost.at<float>(5), iHeight - beleif.y);
		
		Point belCenter;
		belCenter.x = beleif.x + (beleif.width * 0.5);
		belCenter.y = beleif.y + (beleif.height * 0.5);

		double belRad = sqrt(pow(beleif.width,2) + pow(beleif.height,2)) * 0.5;

//		double faceUnc = norm(KFTracker->errorCovPost, NORM_L2);
		double faceUncPos = sqrt(
				pow(KFTracker->errorCovPost.at<float>(0,0), 2) +
				pow(KFTracker->errorCovPost.at<float>(1,1), 2) +
				pow(KFTracker->errorCovPost.at<float>(4,4), 2) +
				pow(KFTracker->errorCovPost.at<float>(5,5), 2) 
				);
		
        if ((debugLevel & 0x02) == 0x02)
        {
            txtstr.str("");
            txtstr << "P:" << std::setprecision(3) << faceUncPos << " S:" << beleif.width << "x" << beleif.height;
            putText(debugFrame, txtstr.str(), belCenter + Point(0, 50), FONT_HERSHEY_PLAIN, 2, CV_RGB(255,0,0));

            circle(debugFrame, belCenter, belRad, CV_RGB(255,0,0));
            circle(debugFrame, belCenter, (belRad - faceUncPos < 0) ? 0 : (belRad - faceUncPos), CV_RGB(255,255,0));
            circle(debugFrame, belCenter, belRad + faceUncPos, CV_RGB(255,0,255));
        }
			
		searchROI.x = max<int>(belCenter.x - KFTracker->statePost.at<float>(4) * 2, 0);
		searchROI.y = max<int>(belCenter.y - KFTracker->statePost.at<float>(5) * 2, 0);
        int x2 = min<int>(belCenter.x + KFTracker->statePost.at<float>(4) * 2, iWidth);
        int y2 = min<int>(belCenter.y + KFTracker->statePost.at<float>(4) * 2, iHeight);
		searchROI.width = x2 - searchROI.x;
		searchROI.height = y2 - searchROI.y;
	
        
		if ((updateFaceHist) && (skinEnabled))
		{
            //updateFaceHist is true when we see a real face (not all the times)
            Rect samplingWindow;
//            samplingWindow.x = beleif.x + (0.25 * beleif.width);
//            samplingWindow.y = beleif.y + (0.1 * beleif.height);
//            samplingWindow.width = beleif.width * 0.5;
//            samplingWindow.height = beleif.height * 0.9;
            samplingWindow.x = measurement.at<float>(0) + (0.25 * measurement.at<float>(2));
            samplingWindow.y = measurement.at<float>(1) + (0.10 * measurement.at<float>(3));
            samplingWindow.width = measurement.at<float>(2) * 0.5;
            samplingWindow.height = measurement.at<float>(3) * 0.9;
            if ((debugLevel & 0x04) == 0x04)
            {
                rectangle(debugFrame, samplingWindow, CV_RGB(255,0,0));
            }
			Mat _face = rawFrame(samplingWindow);
			generateRegionHistogram(_face, faceHist);
		}
		
		
	}

    if ((debugLevel & 0x02) == 0x02)
    {
        rectangle(debugFrame, searchROI, CV_RGB(0,0,0));
        txtstr.str("");
        txtstr << strStates[trackingState] << "(" << std::setprecision(3) << (dt * 1e3) << "ms )";
        putText(debugFrame, txtstr.str() , Point(30,300), FONT_HERSHEY_PLAIN, 2, CV_RGB(255,255,255));
    }
	
//	dt =  ((double) getTickCount() - t) / ((double) getTickFrequency()); // In Seconds	
}

void CHumanTracker::trackSkin()
{
    
    bool perform = 
        (skinEnabled) &&
        (
            (trackingState == STATE_TRACK) || 
            (trackingState == STATE_REJECT)
        );
    
    if (perform)
    {
        histFilter(rawFrame, skin, skinPrior, faceHist, false);
        skinPrior = skin.clone();
        if ((debugLevel & 0x04) == 0x04)
        {
            Mat visSkin;
            normalize(skin, visSkin, 1.0, 0.0, NORM_MINMAX);
            visSkin.convertTo(skinFrame, CV_8UC1, 255.0, 0.0);
        }
    }
}

void CHumanTracker::calcOpticalFlow()
{
	static bool first = true;
	static bool firstCancel = true;
	static Rect prevFaceRect;
	
	bool perform = 
        (true) &&
        (
            (trackingState == STATE_TRACK) || 
            (trackingState == STATE_REJECT)
        );
	
	if (!perform) return;
	
	if (first)
	{
		first = false;
		cvtColor(rawFrame, prevRawFrameGray, CV_BGR2GRAY);
		return;
	}
	
	Point belCenter;
	belCenter.x = beleif.x + (beleif.width * 0.5);
	belCenter.y = beleif.y + (beleif.height * 0.5);
	flowROI.x = max<int>(belCenter.x - KFTracker->statePost.at<float>(4) * 4, 0);
	flowROI.y = max<int>(belCenter.y - KFTracker->statePost.at<float>(5) * 3, 0);
	int x2 = min<int>(belCenter.x + KFTracker->statePost.at<float>(4) * 4, iWidth);
	int y2 = min<int>(belCenter.y + KFTracker->statePost.at<float>(4) * 2, iHeight);
	flowROI.width = x2 - flowROI.x;
	flowROI.height = y2 - flowROI.y;
	
	cvtColor(rawFrame, rawFramGray, CV_BGR2GRAY);
	
	bool cancelCameraMovement = (trackingState == STATE_TRACK);	

	// TODO: Optimization here
	Mat _i1 = prevRawFrameGray(flowROI);
	Mat _i2 = rawFramGray(flowROI);
	//calcOpticalFlowFarneback( _i1, _i2 , flow, 0.5, 3, 50, 3, 9, 1.9, 0);//OPTFLOW_USE_INITIAL_FLOW);		
	calcOpticalFlowFarneback( _i1, _i2 , flow, 0.5, 3, 5, 3, 9, 1.9, 0);//OPTFLOW_USE_INITIAL_FLOW);		
	std::vector<Mat> flowChannels;
	split(flow, flowChannels);
//	magnitude(flowChannels[0], flowChannels[1], flowMag);		
//	threshold(flowMag, flowMag, minFlow, 0.0, THRESH_TOZERO);
//	normalize(flowMag, flowMag, 0.0, 1.0, NORM_MINMAX);

	if (true) //(cancelCameraMovement)
	{
		/* Experimental */
		// This mask is very important because zero-valued elements
		// Should not be taken into account
		Mat maskX;
		Mat maskY;

		maskX = abs(flowChannels[0]) > 0.01;
		maskY = abs(flowChannels[1]) > 0.01;
					
		double minX, minY, maxX, maxY;
		minMaxLoc(flowChannels[0], &minX, &maxX, 0, 0, maskX);
		minMaxLoc(flowChannels[1], &minY, &maxY, 0, 0, maskY);
		
		float medX = calcMedian(flowChannels[0], 25, minX, maxX, maskX);
		float medY = calcMedian(flowChannels[1], 25, minY, maxY, maskY);
	
//		ROS_INFO("Number of channls: %d", flowChannels.size());
//		ROS_INFO("X: <%6.4lf..%6.4lf> Y: <%6.4lf..%6.4lf>", minX, maxX, minY, maxY);
//		ROS_INFO("Median X: %6.4f Y: %6.4f", medX, medY);
		
		add(flowChannels[0], Scalar::all(-medX), flowChannels[0], maskX);			
		add(flowChannels[1], Scalar::all(-medY), flowChannels[1], maskY);

	}
	magnitude(flowChannels[0], flowChannels[1], flowMag);
	threshold(flowMag, flowMag, minFlow, 0.0, THRESH_TOZERO);
	normalize(flowMag, flowMag, 0.0, 1.0, NORM_MINMAX);
		
	float biasInFlow;	
	Rect r;
	if (false)//(cancelCameraMovement)
	{		
		if (firstCancel)
		{
			prevFaceRect = faces[0];
			firstCancel = false;
		}
		else
		{
			Mat curFace = rawFramGray(faces[0]);
			Mat prevFace = prevRawFrameGray(prevFaceRect);
			prevFaceRect = faces[0];
		}
		r = faces[0];
		r.x = faces[0].x - flowROI.x;
		r.y = faces[0].y - flowROI.y;		
		biasInFlow = mean(flowMag(r))[0];
		
		Point2d fCenter;				
		fCenter.x = (r.x + r.width/2.0);
		fCenter.y = (r.y + r.height/2.0);
		for (int y = 0; y < flowMag.rows; y++)
		{
			for (int x = 0; x < flowMag.cols; x++)
			{
				float rad = sqrt( pow(x - fCenter.x, 2) + pow(y - fCenter.y, 2));
				if (fabs(flowMag.at<float>(y,x)) > 0.01)
				{
					flowMag.at<float>(y,x) = max<float>(0.0, flowMag.at<float>(y,x) - (rad * biasInFlow));
//					flowMag.at<float>(y,x) = max<float>(0.0, flowMag.at<float>(y,x) - (1.0 * biasInFlow));
				}
			}
		}
//		flowMag = flowMag - Scalar::all(biasInFlow);
//		ROS_INFO("Bias is %4.2f", biasInFlow);
	}
	std::swap(prevRawFrameGray, rawFramGray);
	
	// Gesture regions update	
	gestureRegion[REG_TOPLEFT].x = flowROI.x;
	gestureRegion[REG_TOPLEFT].y = flowROI.y;
	gestureRegion[REG_TOPLEFT].width = belCenter.x  - flowROI.x;
	gestureRegion[REG_TOPLEFT].height = belCenter.y  - flowROI.y + (beleif.height * 1.5);

	gestureRegion[REG_TOPRIGHT].x = belCenter.x;
	gestureRegion[REG_TOPRIGHT].y = flowROI.y;
	gestureRegion[REG_TOPRIGHT].width = (flowROI.x + flowROI.width) - belCenter.x;
	gestureRegion[REG_TOPRIGHT].height = gestureRegion[REG_TOPLEFT].height;
	
	gestureRegion[REG_BOTLEFT].x = gestureRegion[REG_TOPLEFT].x;
	gestureRegion[REG_BOTLEFT].y = flowROI.y + gestureRegion[REG_TOPLEFT].height + beleif.height;
	gestureRegion[REG_BOTLEFT].width = gestureRegion[REG_TOPLEFT].width;
	gestureRegion[REG_BOTLEFT].height = flowROI.y + flowROI.height - gestureRegion[REG_BOTLEFT].y;
	
	gestureRegion[REG_BOTRIGHT].x = gestureRegion[REG_TOPRIGHT].x;
	gestureRegion[REG_BOTRIGHT].y = gestureRegion[REG_BOTLEFT].y;
	gestureRegion[REG_BOTRIGHT].width = gestureRegion[REG_TOPRIGHT].width;
	gestureRegion[REG_BOTRIGHT].height = gestureRegion[REG_BOTLEFT].height;
	
	// Not using two bottom regions
	for (int i = 0; i < 2; i++)
	{
		safeRectToImage(gestureRegion[i]);
		Rect reg = gestureRegion[i];
		reg.x = gestureRegion[i].x - flowROI.x;
		reg.y = gestureRegion[i].y - flowROI.y;		
		
		float sFlow = ((gestureRegion[i].width > 0) && (gestureRegion[i].height > 0)) ? sum(flowMag(reg))[0] : 0.0;
		//flowScoreInRegion[i] = (0.5 * flowScoreInRegion[i]) + (0.5 * sFlow);
		// No filtering should be done here
		flowScoreInRegion[i] = sFlow;
	}
	
	// Visulization	
	if ((debugLevel & 0x10) == 0x10)
	{
		std::vector<Mat> channels;		
		split(rawFrame(flowROI), channels);
		
		// HSV Color Space
		// Red to blue specterum is between 0->120 degrees (Red:0, Blue:120)
		flowMag.convertTo(channels[0], CV_8UC1, 120);
		channels[0] = Scalar::all(120) - channels[0];
		//flowMag = 120 - flowMag;
		channels[1] = Scalar::all(255.0);
		channels[2] = _i2;//Scalar::all(255.0);
		Mat _tmp = opticalFrame(flowROI);
		merge(channels, _tmp);
		cvtColor(opticalFrame, opticalFrame, CV_HSV2BGR);		
	}
	
	if ((debugLevel & 0x02) == 0x02)
	{
		for (int i = 0; i < 4; i++)
		{
			std::stringstream txtstr;
			rectangle(debugFrame, gestureRegion[i], CV_RGB(0,255,0));
			Point2d center;
			center.x = gestureRegion[i].x + gestureRegion[i].width / 2;
			center.y = gestureRegion[i].y + gestureRegion[i].height / 2;
			txtstr << fixed << setprecision(3) << flowScoreInRegion[i];
			putText(debugFrame, txtstr.str(), center, FONT_HERSHEY_PLAIN, 1, CV_RGB(0,0,255));
		}
	}
	
	/* Experimental*/
//	if ((isFaceInCurrentFrame) && (trackingState == STATE_TRACK))
//	{
//		int minHessian = 300;
//		SurfFeatureDetector detector( minHessian );
//		std::vector<KeyPoint> keypoints;
//		detector.detect( rawFramGray(faces[0]), keypoints );
//		
//		Mat _img = debugFrame(faces[0]);
//		drawKeypoints( _img, keypoints, _img, Scalar::all(-1), DrawMatchesFlags::DEFAULT );
//	}
	
}

void CHumanTracker::draw()
{
    return ;    
}

void CHumanTracker::visionCallback(const sensor_msgs::ImageConstPtr& frame)
{
	tStart = ros::Time::now();
	cv_bridge::CvImagePtr cv_ptr;    
    try
    {
		shouldPublish = true;
        cv_ptr = cv_bridge::toCvCopy(frame, sensor_msgs::image_encodings::BGR8);
    }
    catch (cv_bridge::Exception& e)
    {
        ROS_ERROR("Error converting the input image: %s", e.what());
        return;
    }
	
	if (isInited == false)
	{
		// Get Image Info from the first frame
		isInited = true;
		iWidth = cv_ptr->image.cols;
		iHeight = cv_ptr->image.rows;
        ROS_INFO("Image size is %d x %d", iWidth, iHeight);
		reset();
	}
	
	//TODO: Check clone
	this->rawFrame = cv_ptr->image;
	
	tFaceStart = ros::Time::now();
	detectAndTrackFace();
	
	tSkinStart = ros::Time::now();
    trackSkin();
	
	tFlowStart = ros::Time::now();
	calcOpticalFlow();

	tEnd = ros::Time::now();

	std::stringstream txtstr;
	if ((debugLevel & 0x02) == 0x02)
    {
        txtstr.str("");
		txtstr << fixed;
        txtstr << "Ca:" << "(" << std::setprecision(1) << ((tFaceStart - tStart).toSec() * 1e3) << "ms)";
		txtstr << " Fa:" << "(" << std::setprecision(1) << ((tSkinStart - tFaceStart).toSec() * 1e3) << "ms)";
		txtstr << " Sk:" << "(" << std::setprecision(1) << ((tFlowStart - tSkinStart).toSec() * 1e3) << "ms)";
		txtstr << " Fl:" << "(" << std::setprecision(1) << ((tEnd - tFlowStart).toSec() * 1e3) << "ms)";
		txtstr << " T:" << "(" << std::setprecision(1) << ((tEnd - tStart).toSec() * 1e3) << "ms)";
        putText(debugFrame, txtstr.str() , Point(30,330), FONT_HERSHEY_PLAIN, 1, CV_RGB(255,255,255));		
    }
}

int main(int argc, char **argv)
{
	ros::init(argc, argv, "autonomy_human");
	ros::NodeHandle n;
	image_transport::ImageTransport it(n);
		
	//TODO: Get from rosparam
	string xmlFile = "../cascades/haarcascade_frontalface_default.xml";
    
    //CHumanTracker* humanTracker = new CHumanTracker(xmlFile, 5, 6, 6, true, 0x06);
    
    std::string paramName;
    
    paramName.assign("~skin_enabled");
    bool skinEnabled;
    if (false == ros::param::get( paramName, skinEnabled))
        skinEnabled = false;
    
    paramName.assign("~debug_mode");
    int  debugMode;
    if (false == ros::param::get( paramName, debugMode))
        debugMode = 0x02;
    
	CHumanTracker* humanTracker = new CHumanTracker(xmlFile, 5, 6, 6, 10, skinEnabled, debugMode);
	
	// Median Test
//	Mat test = Mat::zeros(1, 10, CV_32FC1);
//	test = * (Mat_<float>(1,10) 
//			<< 
//			0.0, 0.0, 0.0, 0.0, 0.0,
//			0.2, 0.2, 0.2, 0.2, 0.8
//			);
//	ROS_INFO("%f :", humanTracker->calcMedian(test, 10, 1.0, true));

	/**
	 * The queue size seems to be very important in this project
	 * If we can not complete the calculation in the 1/fps time slot, we either 
	 * drop the packet (low queue size) or process all frames which will lead to
	 * additive delays.
	 */ 
	
	image_transport::Subscriber visionSub = it.subscribe("input_rgb_image", 1, &CHumanTracker::visionCallback, humanTracker);
	ros::Publisher facePub = n.advertise<autonomy_human::human>("human", 5);  
    image_transport::Publisher debugPub = it.advertise("output_rgb_debug", 1);
    image_transport::Publisher skinPub = it.advertise("output_rgb_skin", 1);
	image_transport::Publisher opticalPub = it.advertise("output_rgb_optical", 1);

	
	ROS_INFO("Starting Autonomy Human ...");
	
	ros::Rate loopRate(50);
	autonomy_human::human msg;
    
    cv_bridge::CvImage cvi;
    sensor_msgs::Image im;
    cvi.header.frame_id = "image";
    
	while (ros::ok()){
		
		if (
				(humanTracker->trackingState == CHumanTracker::STATE_TRACK) ||
				(humanTracker->trackingState == CHumanTracker::STATE_REJECT)
			)
		{
			msg.header.stamp = ros::Time::now();
			msg.numFaces = humanTracker->faces.size();
			msg.faceScore = humanTracker->faceScore;
			msg.faceROI.x_offset = humanTracker->beleif.x;
			msg.faceROI.y_offset = humanTracker->beleif.y;
			msg.faceROI.width = humanTracker->beleif.width;
			msg.faceROI.height = humanTracker->beleif.height;
			for (int i = 0; i < 4; i++)
				msg.flowScore[i] = humanTracker->flowScoreInRegion[i];
			facePub.publish(msg);
		}
        
        if (
//                (debugPub.getNumSubscribers() > 0) &&
				(humanTracker->shouldPublish) &&
                ((debugMode & 0x02) == 0x02) &&
                (humanTracker->isInited)
           )
        {
            cvi.header.stamp = ros::Time::now();
            cvi.encoding = "bgr8";
            cvi.image = humanTracker->debugFrame;
            cvi.toImageMsg(im);
            debugPub.publish(im);
        }
        
        if (
//                (skinPub.getNumSubscribers() > 0) && 
				(humanTracker->shouldPublish) &&
                ((debugMode & 0x04) == 0x04) &&
                (humanTracker->isInited) &&
                (skinEnabled)
           )
        {
            cvi.header.stamp = ros::Time::now();
            cvi.encoding = "mono8";
            cvi.image = humanTracker->skinFrame;
            cvi.toImageMsg(im);
            skinPub.publish(im);
        }
		
		if (
				(humanTracker->shouldPublish) &&
                ((debugMode & 0x10) == 0x10) &&
                (humanTracker->isInited)
           )
        {
            cvi.header.stamp = ros::Time::now();
            cvi.encoding = "bgr8";
            cvi.image = humanTracker->opticalFrame;
            cvi.toImageMsg(im);
            opticalPub.publish(im);
        }
		        
		ros::spinOnce();
		loopRate.sleep();
	}
	
	delete humanTracker;
	return 0;
}

