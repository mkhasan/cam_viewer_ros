
#include "cam_viewer/cam_viewer.h"

#include "client_interface/shm_manager.h"


#include "ros/ros.h"
#include "std_msgs/String.h"
#include "spectator/empty.h"



#include <ace/Shared_Memory_SV.h>

#include <iostream>
#include <sstream>
#include <string>
#include <string.h>
#include <assert.h>

using namespace std;
using namespace client_interface;


int quit=0;
shm_data *pData;

sem_t *mutex;
sem_t *debugLock;

pthread_t cam_viewer_thread;

string mutexPrefix;

void * cam_viewer(void * ptr);



extern "C" {
void debug_print(const char * str) {
	ROS_DEBUG(str);

}

void info_print(const char * str) {
	ROS_INFO(str);
}

void warn_print(const char * str) {
	ROS_WARN(str);
}

void error_print(const char * str) {
	ROS_ERROR(str);
}

}



void mySigintHandler(int sig)
{

	ROS_WARN("Signal %d caught", sig);

	if(sig == SIGINT) {
		ROS_WARN("SIGINT caught");


	}


	quit = QUIT;
	pthread_join( cam_viewer_thread, NULL);



	ros::Duration(1.0).sleep();
	ros::shutdown();
}


bool test_only(spectator::empty::Request &req, spectator::empty::Response &resp) {

	quit = TEST_ONLY;
	return true;
}


int main(int argc, char **argv) {


	ros::init(argc, argv, "cam_viewer_node", ros::init_options::NoSigintHandler);

	ros::NodeHandle nh;

	int delay_on_start=0;
	int id=0;



	nh.param("/CAM_VIEWER/delay_on_start", delay_on_start, 0);
	//n.param("/CAM_VIEWER/my_id", id, 0);
	nh.param("/ROBOT/MUTEX_PREFIX", mutexPrefix, string("kict_mp_camera00_"));

	ROS_ERROR("MUTEX PREFIX IS %s", mutexPrefix.c_str());

	if (argc > 1)
		id = stoi(argv[1]);

	if( ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Warn) ) {
	   ros::console::notifyLoggerLevelsChanged();
	}


	if(argc > 1) {
		ROS_DEBUG("id is %d", id);
	}


	ros::ServiceServer service1 = nh.advertiseService("cam_viewre/test_only", test_only);


	//return 0;


	signal(SIGTERM, mySigintHandler);
	signal(SIGINT, mySigintHandler);          // caught in a different way fo$
	signal(SIGHUP, mySigintHandler);
	signal(SIGKILL, mySigintHandler);
	signal(SIGTSTP, mySigintHandler);


#ifdef TEST_
	delay_on_start = 0;
#endif

	for (int k=5*delay_on_start; k>0; k--) {

		ROS_DEBUG("Wait for %d more secs", k);
		ros::Duration(1.0).sleep();

	}


	//return 0;

	if(id < 0 || id >= ShmManager::MAX_NUM_CAMERA) {
			cerr << "id " << id << " out of range [0 ..." << ShmManager::MAX_NUM_CAMERA-1 << "]" << endl;
			exit(-1);
		}














	int iret = pthread_create( &cam_viewer_thread, NULL, cam_viewer, (void *) &id);
    if(iret) {
        ROS_ERROR("%d: Error in creating cam viewer", id);
        return -1;
    }

	while (ros::ok()) {


		ros::spinOnce();

		ros::Duration(5.0).sleep();
	}

/*
	for(int i=0; i<MAX_PROCESS; i++) {
		if(pid[i]) {
			kill(pid[i], 2);
			pid[i] = 0;
		}
	}

*/

	return 0;
}


void * cam_viewer(void * ptr) {
	int id = *((int *) ptr);




#ifndef TEST_
    debugLock = sem_open(mutexPrefix.c_str(), O_RDWR);
    if (debugLock == SEM_FAILED) {
        cerr << "sem_open(debugLock) failed" << endl;
        exit(-1);
    }

#else
    int k;

    for (k=0; k<100; k++) {
    	stringstream ss;
    	ss << "DEBUG_" << k;
    	string str = ss.str();

   		debugLock = sem_open(str.c_str(), O_CREAT | O_EXCL, SEM_PERMS, INITIAL_VALUE);
   		if (debugLock)
   			break;
    }

    if(k == 100) {
    	ROS_ERROR("FAiled to make debugLock");
    	exit(1);
    }

    ROS_WARN("debugLock created with id %s%d", "DEBUG_", k);

#endif




	string mutexName;
	stringstream ss;
	ss << id;
	mutexName = mutexPrefix+ss.str();


#ifndef TEST_
    mutex = sem_open(mutexName.c_str(), O_RDWR);
    if (mutex == SEM_FAILED) {
        cerr << "sem_open(3) failed" << endl;
        exit(-1);
    }
#else

    for (k=0; k<100; k++) {
    	stringstream ss;
    	ss << "MUTEX_" << k;
    	string str = ss.str();

   		mutex = sem_open(str.c_str(), O_CREAT | O_EXCL, SEM_PERMS, INITIAL_VALUE);
   		if (mutex)
   			break;
    }

    if(k == 100) {
    	ROS_ERROR("FAiled to make mutex");
    	exit(1);
    }

    ROS_WARN("mutex created with id %s%d", "MUTEX_", k);

#endif


    ACE_Shared_Memory_SV shm_client(ShmManager::SHM_KEY_START+id, sizeof (shm_data));

    char *shm = (char *) shm_client.malloc ();

    if (shm == NULL) {
    	sem_close(mutex);
    	sem_close(debugLock);
    	cerr << "Error in initilizing shared memeory " << endl;
    	return NULL;
    }

    pData = new (shm) shm_data;

#ifdef TEST_
    pData->len = MAX_DATA_SIZE+1;
#endif

	if(sem_wait(mutex) < 0) {
		cerr << "error in getting mutex" << endl;
		exit(-1);
	}


	//strcpy(camUrl, pData->url);

	if(!(pData->len > MAX_DATA_SIZE) || strlen(pData->url) == 0)	 {
		bool flag = strlen(pData->url) == 0;
		sem_post(mutex);
		sem_close(mutex);

		if (flag)
			cerr << "empty url given" << endl;
		else
			cerr << "shared data occupied" << endl;
		exit(1);
	}
	pData->len = 0;			// image processor joined

	if(sem_post(mutex) < 0) {
		cerr << "error in getting mutex" << endl;
		exit(-1);
	}

	//return 0;

	cam_info_t camInfo;
	camInfo.id = id;
	camInfo.mutex = mutex;
	camInfo.debugLock = debugLock;
	camInfo.p = pData;
	camInfo.quit = &quit;

	int ret=0;
	while(1) {

		ret = play(camInfo);

		if(ret != 0) {
			ROS_ERROR("%d: Abnormal termination of function play", id);
		}
		if (quit == FRAME_READ_ERROR || quit == TEST_ONLY) {
			ROS_ERROR("%d: Frame read error occured", id);
			usleep(1000000);
			quit = NO_QUIT;
			continue;
		}

		break;

	}

	ROS_WARN("%d: Terminating with code %d", id, quit);

	if(ret != 0) {
		cerr << "Error in playing stream " << (pData != NULL ? pData->url : "")  << endl;
	}

	if(sem_wait(mutex) < 0) {
		cerr << "error in getting mutex" << endl;
		exit(-1);
	}

	pData->len = MAX_DATA_SIZE+1;			// image processor left

	if(sem_post(mutex) < 0) {
		cerr << "error in getting mutex" << endl;
		exit(-1);
	}
	sem_close(mutex);
	sem_close(debugLock);

	return NULL;
}
