/*
 * cam_viewer.h
 *
 *  Created on: Feb 17, 2019
 *      Author: kict
 */

#ifndef CATKIN_WS_SRC_CAM_VIEWER_INCLUDE_CAM_VIEWER_CAM_VIEWER_H_
#define CATKIN_WS_SRC_CAM_VIEWER_INCLUDE_CAM_VIEWER_CAM_VIEWER_H_

#include "client_interface/shm_data.h"

#include <semaphore.h>

//#define TEST_

#ifdef TEST_

#define SEM_PERMS (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#define INITIAL_VALUE 1

#endif


#define NO_QUIT 0
#define QUIT 1
#define TERMINATE 2
#define FRAME_READ_ERROR 3
#define DEVICE_ERROR 4
#define TEST_ONLY 5


#ifdef __cplusplus
extern "C" {
#endif

#include "client_interface/utils.h"

typedef struct camera_info {
	struct shm_data *p;
	int id;
	sem_t *mutex;
	sem_t *debugLock;
	int *quit;
} cam_info_t;


int play(cam_info_t info);

#ifdef __cplusplus
}
#endif




#endif /* CATKIN_WS_SRC_CAM_VIEWER_INCLUDE_CAM_VIEWER_CAM_VIEWER_H_ */
