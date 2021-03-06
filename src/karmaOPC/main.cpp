/* 
 * Copyright (C) 2012 Department of Robotics Brain and Cognitive Sciences - Istituto Italiano di Tecnologia
 * Author: Francesca Stramandinoli
 * email:  francesca.stramandinoli@iit.it
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

#include <string>
#include <cmath>
#include <algorithm>
#include <cv.h>
#include <highgui.h>
#include <opencv2/objdetect/objdetect.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <opencv2/opencv.hpp>
#include <yarp/os/all.h>
#include <yarp/sig/all.h>


using namespace cv;
using namespace std;
using namespace yarp::os;
using namespace yarp::sig;


#define RET_INVALID -1

/*****************************************************/
class KarmaOPC : public RFModule
{

	string													name; //name of the module
    string                                                  arm;

    RpcClient												opcPort;
    RpcClient												motorPort;
    RpcClient												areCmdPort;
    RpcClient												areGetPort;

	Port													trackOutPort;
		
	BufferedPort<Bottle>									blobsPort;
    BufferedPort<Bottle>									trackInPort;
	BufferedPort<yarp::sig::ImageOf<yarp::sig::PixelRgb> >	imagePort;
    BufferedPort<Bottle>	                                logPort;

    RpcServer												rpcPort;

	yarp::os::Mutex mutex;

    /*****************************************************/
    bool get3DObjLoc(const string &objName, yarp::sig::Vector &x,
		yarp::sig::Vector &bbox)
    {
        if (opcPort.getOutputCount()>0)
        {
			//Format: [ask] (("prop0" "<" <val0>) || ("prop1" ">=" <val1>) ...) 
            Bottle opcCmd,opcReply,opcReplyProp;
            opcCmd.addVocab(Vocab::encode("ask"));
            Bottle &content=opcCmd.addList();
            Bottle &cond_1=content.addList();
            cond_1.addString("entity");
            cond_1.addString("==");
            cond_1.addString("object");
			content.addString("&&");
            Bottle &cond_2=content.addList();
            cond_2.addString("name");
            cond_2.addString("==");
            cond_2.addString(objName.c_str());

			yDebug("opc cmd is %s \n", opcCmd.toString().c_str());

            opcPort.write(opcCmd,opcReply);

			yDebug("opc reply is %s \n", opcReply.toString().c_str());

            if (opcReply.size()>1)
            {
                if (opcReply.get(0).asVocab()==Vocab::encode("ack"))
                {
                    if (Bottle *idField=opcReply.get(1).asList())
                    {
                        if (Bottle *idValues=idField->get(1).asList())
                        {
                            int id=idValues->get(0).asInt();

							yDebug("object ID is %i \n", id);

                            // get the relevant properties
                            // Format: [get] (("id" <num>) ("propSet" ("position_3d" "position_2d_left")))
                            opcCmd.clear();
                            opcCmd.addVocab(Vocab::encode("get"));
                            Bottle &content=opcCmd.addList();
                            Bottle &list_bid=content.addList();
                            list_bid.addString("id");
                            list_bid.addInt(id);
                            Bottle &list_propSet=content.addList();
                            list_propSet.addString("propSet");
							Bottle &list_props=list_propSet.addList();
							list_props.addString("position_3d");
							list_props.addString("position_2d_left");
                            list_props.addString(("kinematic_offset_"+arm).c_str());

							cout << "Query list " << opcCmd.toString().c_str() << endl;
							
							opcPort.write(opcCmd,opcReplyProp);
                            if (opcReplyProp.get(0).asVocab()==Vocab::encode("ack"))
                            {
                                if (Bottle *propField=opcReplyProp.get(1).asList())
                                {
									cout << propField->size() << endl;
									cout << propField->toString().c_str();

									int cnt=0;
                                    if (Bottle *b=propField->find("position_3d").asList())
                                    {
                                        x.resize(3);
                                        for (int i=0; i< b->size(); i++)
                                            x[i]=b->get(i).asDouble();
                                        cnt++;
                                    }

                                    if (Bottle *b=propField->find("position_2d_left").asList())
                                    {
                                        bbox.resize(4);
                                        for (int i=0; i< b->size(); i++)
                                            bbox[i]=b->get(i).asDouble();
                                        cnt++;
                                    }

                                    if (Bottle *b=propField->find(("kinematic_offset_"+arm).c_str()).asList())
                                    {
                                        if (cnt>=2)
                                            for (int i=0; i< b->size(); i++)
                                                x[i]+=b->get(i).asDouble();
                                    }

                                    return (cnt>=2);
                                }
                            }
                        }
                    }
                }
            }
        }

        return false;
    }

    /*****************************************************/
	bool get3DPosition(const yarp::sig::Vector &point, yarp::sig::Vector &x)
    {
        Bottle are_cmd,are_rep;
        are_cmd.addVocab(Vocab::encode("get"));
        are_cmd.addVocab(Vocab::encode("s2c"));
        Bottle &options=are_cmd.addList();
        options.addString("left");
        options.addInt((int)point[0]);
        options.addInt((int)point[1]);

        yDebug()<<"get3DPosition query:"<<are_cmd.toString();
        areGetPort.write(are_cmd,are_rep);
        yDebug()<<"get3DPosition response:"<<are_rep.toString();
        if (Bottle *b=are_rep.get(0).asList())
        {   
            if (b->size()>=3)
            {
                x.resize(3);
                x[0]=b->get(0).asDouble();
                x[1]=b->get(1).asDouble();
                x[2]=b->get(2).asDouble();
                return true;
            }
            else
                return false;
        }
        else
            return false;
    }

	/**********************************************************/
	CvPoint getBlobCOG(const Bottle &blobs, const int i)
	{
		CvPoint cog = cvPoint(RET_INVALID, RET_INVALID);
		if ((i >= 0) && (i<blobs.size()))
		{
			CvPoint tl, br;
			Bottle *item = blobs.get(i).asList();
			if (item == NULL)
				return cog;

			tl.x = (int)item->get(0).asDouble();
			tl.y = (int)item->get(1).asDouble();
			br.x = (int)item->get(2).asDouble();
			br.y = (int)item->get(3).asDouble();

			cog.x = (tl.x + br.x) >> 1;
			cog.y = (tl.y + br.y) >> 1;
		}
		return cog;
	}

	/**********************************************************/
	Bottle findClosestBlob(const Bottle &blobs, const CvPoint &loc)
	{
		int ret = RET_INVALID;
		double min_d2 = std::numeric_limits<double>::max();
		Bottle pointReturn;
		pointReturn.clear();
		for (int i = 0; i<blobs.size(); i++)
		{
			CvPoint cog = getBlobCOG(blobs, i);
			if ((cog.x == RET_INVALID) || (cog.y == RET_INVALID))
				continue;

			double dx = loc.x - cog.x;
			double dy = loc.y - cog.y;
			double d2 = dx*dx + dy*dy;

			if (d2<min_d2)
			{
				min_d2 = d2;
				ret = i;
			}
		}
		CvPoint cog = getBlobCOG(blobs, ret);
		pointReturn.addDouble(cog.x);           //cog x
		pointReturn.addDouble(cog.y);           //cog y
		pointReturn.addInt(ret);                //index of blob
		Bottle *item = blobs.get(ret).asList();
		double orient = (double)item->get(4).asDouble();
		int axe1 = item->get(5).asInt();
		int axe2 = item->get(6).asInt();
		pointReturn.addDouble(orient);
		pointReturn.addInt(axe1);
		pointReturn.addInt(axe2);
		return pointReturn;
	}

	/*****************************************************/
	int getRoiAverage(int x, int y)
	{
		int channel = -1;
		
		CvPoint loc;
		loc.x = x;
		loc.y = y;

		//get blob list
		Bottle *blobsList = blobsPort.read(false);
		yDebug("blobs %s \n", blobsList->toString().c_str());

		//get closest blob
		Bottle blob = findClosestBlob(*blobsList, loc);
		yDebug("blob is %s \n", blob.toString().c_str());
		
		//get image
		cv::Mat img((IplImage*)imagePort.read(true)->getIplImage(), true);
		Bottle *item = blobsList->get(blob.get(2).asInt()).asList();

		int blobWidth = abs((int)item->get(2).asDouble() - (int)item->get(0).asDouble());
		int blobHeight = abs((int)item->get(3).asDouble() - (int)item->get(1).asDouble());

		if (blobHeight < 60)
		{
			int blobHeight = abs((int)item->get(3).asDouble() - (int)item->get(1).asDouble());
			
			//get blob mean
			cv::Rect roi( (int)item->get(0).asDouble(), (int)item->get(1).asDouble(), blobWidth, blobHeight) ;

			cv::Mat image_roi = img(roi);
			//cvtColor(image_roi, image_roi, CV_BGR2RGB);

			cv::Scalar avgPixel = cv::mean(image_roi);

			//namedWindow("test", CV_WINDOW_AUTOSIZE);
			//imshow("test", image_roi);
			//waitKey(0);

			yInfo("pixel avg = %lf %lf %lf \n", avgPixel.val[0], avgPixel.val[1], avgPixel.val[2]);

		}
		return channel;
	}

public:
    /*****************************************************/
    bool configure(ResourceFinder &rf)
    {
		name = rf.check("name",Value("stramakarma")).asString().c_str();
        arm = rf.check("arm",Value("right")).asString().c_str();
		
		opcPort.open(("/" + name + "/OPC").c_str());
		motorPort.open(("/" + name + "/motor").c_str());
		areCmdPort.open(("/" + name + "/are/cmd").c_str());
		areGetPort.open(("/" + name + "/are/get").c_str());
        blobsPort.open(("/" + name + "/blobs:i").c_str());
		imagePort.open(("/" + name + "/image:i").c_str());
		trackOutPort.open(("/" + name + "/track:o").c_str());
		trackInPort.open(("/" + name + "/track:i").c_str());
        logPort.open(("/" + name + "/log:o").c_str());
		rpcPort.open(("/" + name + "/rpc").c_str());
        attach(rpcPort);
        
		return true;
    }

    /*****************************************************/
    bool respond(const Bottle &command, Bottle &reply)
    {
        LockGuard lg(mutex);
        string cmd=command.get(0).asString();
        int ack=Vocab::encode("ack");
        int nack=Vocab::encode("nack");

        Bottle &log=logPort.prepare();
        log=command;
		
        bool ret=false;
        if (cmd=="push")
        {
            if (command.size()>=4)
            {
                string objName=command.get(1).asString();
                double theta=command.get(2).asDouble();
                double radius=command.get(3).asDouble();

				yarp::sig::Vector x0, bbox;
                if (get3DObjLoc(objName,x0,bbox))
                {
					// activeParticleTrack
                    Bottle track_cmd;
                    track_cmd.addInt((int)((bbox[0]+bbox[2])/2.0));
                    track_cmd.addInt((int)((bbox[1]+bbox[3])/2.0));
                    trackOutPort.write(track_cmd);

                    // ARE
                    Bottle are_cmd,are_rep;
                    are_cmd.addString("track");
                    are_cmd.addString("track");
                    are_cmd.addString("no_sacc");
                    areCmdPort.write(are_cmd,are_rep);
                    
                    // karmaMotor
                    Bottle motor_cmd,motor_rep;
                    motor_cmd.addString("tool");
                    motor_cmd.addString("attach");
                    motor_cmd.addString(arm.c_str());
                    motor_cmd.addDouble(0.0);
                    motor_cmd.addDouble(0.0);
                    motor_cmd.addDouble(0.0);
                    motorPort.write(motor_cmd,motor_rep);

                    motor_cmd.clear();
                    motor_cmd.addString(cmd);
                    motor_cmd.addDouble(x0[0]);
                    motor_cmd.addDouble(x0[1]);
                    motor_cmd.addDouble(std::max(x0[2],-0.1));
                    motor_cmd.addDouble(theta);
                    motor_cmd.addDouble(radius);
                    motorPort.write(motor_cmd,motor_rep);

                    motor_cmd.clear();
                    motor_cmd.addString("tool");
                    motor_cmd.addString("remove");
                    motorPort.write(motor_cmd,motor_rep);

                    // retrieve displacement
                    if (Bottle *bPoint=trackInPort.read(false))
                    {
                        yDebug()<<"Tracker input:"<<bPoint->toString();
                        if (Bottle *b=bPoint->get(0).asList())
                        {
						    yarp::sig::Vector point(2), x1;
                            point[0]=b->get(1).asInt();
                            point[1]=b->get(2).asInt();

                            if (get3DPosition(point,x1))
                            {
                                reply.addVocab(ack);
                                reply.addDouble(x1[0]-x0[0]);
                                reply.addDouble(x1[1]-x0[1]);
                            }
                            else
                                reply.addVocab(nack);
                        }
                        else
                            reply.addVocab(nack);
                    }
                    else
                        reply.addVocab(nack);

                    // ARE [home]
                    are_cmd.clear();
                    are_cmd.addString("home");
                    are_cmd.addString("all");
                    areCmdPort.write(are_cmd,are_rep);
                }
				else
					reply.addVocab(nack);
            }
			else
				reply.addVocab(nack);
            
            ret=true;
        }
        else
        {
            ret=RFModule::respond(command,reply);
        }

        log.append(reply);
        logPort.writeStrict();
        return ret;
    }

    /*****************************************************/
    double getPeriod()
    {
        return 1.0;
    }

    /*****************************************************/
    bool updateModule()
    {
        LockGuard lg(mutex);
        return true;
    }

    /*****************************************************/
    bool close()
    {
        rpcPort.close();
        logPort.close();
        trackInPort.close();
        trackOutPort.close();
		blobsPort.close();
		imagePort.close();
        areCmdPort.close();
        areGetPort.close();
        motorPort.close();
        opcPort.close();
        return true;
    }
};

/*****************************************************/
int main(int argc, char *argv[])
{
    Network yarp;
    if (!yarp.checkNetwork())
    {
        yError("YARP server not available!");
        return 1;
    }

    ResourceFinder rf;
    rf.setVerbose(true);
    rf.configure(argc,argv);

    KarmaOPC karmaOPC;
    return karmaOPC.runModule(rf);
}



