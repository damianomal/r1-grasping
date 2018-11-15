/******************************************************************************
 *                                                                            *
 * Copyright (C) 2018 Fondazione Istituto Italiano di Tecnologia (IIT)        *
 * All Rights Reserved.                                                       *
 *                                                                            *
 ******************************************************************************/

/**
 * @file main.cpp
 * @authors: Ugo Pattacini <ugo.pattacini@iit.it>
 */

#include <cstdlib>
#include <string>
#include <yarp/os/all.h>
#include <yarp/sig/all.h>
#include <yarp/math/Math.h>

using namespace std;
using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::math;

/****************************************************************/
class Gateway : public RFModule
{
    BufferedPort<ImageOf<PixelFloat>> depthPort;
    BufferedPort<Property> gazePort;
    RpcServer rpcPort;
    RpcClient camPort;

    bool camera_configured;
    double fov_h,fov_v;

    Mutex depthMutex;
    ImageOf<PixelFloat> depth;
    Matrix Hcam;

    /****************************************************************/
    bool getCameraOptions()
    {
        if (camPort.getOutputCount()>0)
        {
            Bottle cmd,rep;
            cmd.addVocab(Vocab::encode("visr"));
            cmd.addVocab(Vocab::encode("get"));
            cmd.addVocab(Vocab::encode("fov"));
            if (camPort.write(cmd,rep))
            {
                if (rep.size()>=5)
                {
                    fov_h=rep.get(3).asDouble();
                    fov_v=rep.get(4).asDouble();
                    yInfo()<<"camera fov_h (from sensor) ="<<fov_h;
                    yInfo()<<"camera fov_v (from sensor) ="<<fov_v;
                    return true;
                }
            }
        }
        return false;
    }

    /****************************************************************/
    Vector getPoint3D(const int u, const int v) const
    {
        Vector p(3,0.0);
        if ((u>=0) && (u<depth.width()) && (v>=0) && (v<depth.height()) && (fov_h>0.0) && (fov_v>0.0))
        {
            double f_h=depth.width()/(2.0*tan(fov_h*(M_PI/180.0)/2.0));
            double f_v=depth.height()/(2.0*tan(fov_v*(M_PI/180.0)/2.0));
            double d=depth(u,v);
            if ((d>0.0) && (f_h>0.0) && (f_v>0.0))
            {
                double x=u-0.5*(depth.width()-1);
                double y=v-0.5*(depth.height()-1);

                p=d*ones(3);
                p[0]*=x/f_h;
                p[1]*=y/f_v;
            }
        }
        return p;
    }

    /****************************************************************/
    bool configure(ResourceFinder &rf) override
    {
        // default values
        camera_configured=false;
        Hcam=eye(4,4);
        fov_h = 0;
        fov_v = 0;

        // retrieve values from config file
        Bottle &gCamera=rf.findGroup("camera");
        if (!gCamera.isNull())
        {
            if (gCamera.check("fov"))
            {
                if (Bottle *fov=gCamera.find("fov").asList())
                {
                    if (fov->size()>=2)
                    {
                        camera_configured=true;
                        fov_h=fov->get(0).asDouble();
                        fov_v=fov->get(1).asDouble();
                        yInfo()<<"camera fov_h (from file) ="<<fov_h;
                        yInfo()<<"camera fov_v (from file) ="<<fov_v;
                    }
                }
            }
        }

        depthPort.open("/vision3d-gateway/depth:i");
        gazePort.open("/vision3d-gateway/gaze/state:i");
        rpcPort.open("/vision3d-gateway/rpc");
        camPort.open("/vision3d-gateway/cam:rpc");

        attach(rpcPort);

        return true;
    }

    /****************************************************************/
    double getPeriod() override
    {
        return 0.0;
    }

    /****************************************************************/
    bool updateModule() override
    {
        if (!camera_configured)
        {
            camera_configured=getCameraOptions();
        }

        if (ImageOf<PixelFloat> *ptr=depthPort.read(true))
        {
            LockGuard lg(depthMutex);
            depth=*ptr;
        }
        if (Property *ptr=gazePort.read(false))
        {
            if (Bottle *pose=ptr->find("depth").asList())
            {
                if (pose->size()>=7)
                {
                    Vector orientation(4);
                    orientation[0]=pose->get(3).asDouble();
                    orientation[1]=pose->get(4).asDouble();
                    orientation[2]=pose->get(5).asDouble();
                    orientation[3]=pose->get(6).asDouble();
                    Hcam=axis2dcm(orientation);
                    Hcam(0,3)=pose->get(0).asDouble();
                    Hcam(1,3)=pose->get(1).asDouble();
                    Hcam(2,3)=pose->get(2).asDouble();
                }
            }
        }
        return true;
    }

    /****************************************************************/
    bool respond(const Bottle &command, Bottle &reply) override
    {
        reply.clear();

        if(command.size()==0)
            return false;

        string cmd=command.get(0).asString();

        if (cmd=="quit")
            return false;
        else if (cmd=="help")
        {
            reply.addVocab(Vocab::encode("many"));
            reply.addString("Available commands are:");
            reply.addString("- [Rect tlx tly w h step]: Given the pixels in the rectangle defined by {(tlx,tly) (tlx+w,tly+h)} (parsed by columns), the response contains the corresponding 3D points in the ROOT frame. The optional parameter step defines the sampling quantum; by default step=1.");
            reply.addString("- [Points u_1 v_1 ... u_n v_n]: Given a list of n pixels, the response contains the corresponding 3D points in the ROOT frame.");
            reply.addString("For more details on the commands, check the module's documentation");
            return true;
        }
        else if ((cmd=="Rect") && (command.size()>=5))
        {
            int tlx=command.get(1).asInt();
            int tly=command.get(2).asInt();
            int w=command.get(3).asInt();
            int h=command.get(4).asInt();
            int step=1;
            if (command.size()>=6)
            {
                step=command.get(5).asInt();
            }

            LockGuard lg(depthMutex);
            for (int u=tlx; u<tlx+w; u+=step)
            {
                for (int v=tly; v<tly+h; v+=step)
                {
                    Vector p=getPoint3D(u,v);
                    p.push_back(1.0);
                    p=Hcam*p;

                    reply.addDouble(p[0]);
                    reply.addDouble(p[1]);
                    reply.addDouble(p[2]);
                }
            }
        }
        else if ((cmd=="Points") && (command.size()>=3))
        {
            LockGuard lg(depthMutex);
            for (int cnt=1; cnt<command.size()-1; cnt+=2)
            {
                int u=command.get(cnt).asInt();
                int v=command.get(cnt+1).asInt();
                Vector p=getPoint3D(u,v);
                p.push_back(1.0);
                p=Hcam*p;

                reply.addDouble(p[0]);
                reply.addDouble(p[1]);
                reply.addDouble(p[2]);
            }
        }
        else
            reply.addString("NACK");
        return true;
    }

    /****************************************************************/
    bool interruptModule() override
    {
        depthPort.interrupt();
        gazePort.interrupt();
        rpcPort.interrupt();
        camPort.interrupt();
        return true;
    }

    /****************************************************************/
    bool close() override
    {
        depthPort.close();
        gazePort.close();
        rpcPort.close();
        camPort.close();
        return true;
    }
};


/****************************************************************/
int main(int argc, char *argv[])
{
    Network yarp;
    if (!yarp.checkNetwork())
    {
        yError()<<"Unable to find Yarp server!";
        return EXIT_FAILURE;
    }

    ResourceFinder rf;
    rf.setDefaultContext("vision3d-gateway");
    rf.setDefaultConfigFile("config.ini");
    rf.configure(argc,argv);

    Gateway gateway;
    return gateway.runModule(rf);
}
