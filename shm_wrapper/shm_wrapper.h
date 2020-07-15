#ifndef SHM_WRAPPER_H
#define SHM_WRAPPER_H

#include <stdio.h>                         //for i/o
#include <stdlib.h>                        //for standard utility functions (exit, sleep)
#include <stdint.h>                        //for standard integer types
#include <sys/types.h>                     //for sem_t and other standard system types
#include <sys/stat.h>                      //for some of the flags that are used (the mode constants)
#include <fcntl.h>                         //for flags used for opening and closing files (O_* constants)
#include <unistd.h>                        //for standard symbolic constants
#include <semaphore.h>                     //for semaphores
#include <sys/mman.h>                      //for posix shared memory
#include "../logger/logger.h"              //for logger (TODO: consider removing relative pathname in include)
#include "../runtime_util/runtime_util.h"  //for runtime constants (TODO: consider removing relative pathname in include)

//enumerated names for the two associated blocks per device
typedef enum stream { DATA, COMMAND } stream_t;

//shared memory has these parts in it
typedef struct dev_shm {
	uint32_t catalog;                                   //catalog of valid devices
	uint32_t pmap[MAX_DEVICES + 1];                     //param bitmap is 17 32-bit integers (changed devices and changed params of devices)
	param_val_t params[2][MAX_DEVICES][MAX_PARAMS];     //all the device parameter info, data and commands
	dev_id_t dev_ids[MAX_DEVICES];                      //all the device identification info
} dev_shm_t;

//two mutex semaphores for each device
typedef struct sems {
	sem_t *data_sem;        //semaphore on the data stream of a device
	sem_t *command_sem;     //semaphore on the command stream of a device
} dual_sem_t;

//shared memory for gamepad
typedef struct gp_shm {
	uint32_t buttons;       //bitmap for which buttons are pressed
	float joysticks[4];     //array to hold joystick positions
} gamepad_shm_t;

//shared memory for robot description
typedef struct robot_desc_shm {
	uint8_t fields[NUM_DESC_FIELDS];   //array to hold the robot state (each is a uint8_t) 
} robot_desc_shm_t;

// ******************************************* PRINTING UTILITIES ***************************************** //

/*
 * Prints the current values in the param bitmap
 */
void print_pmap ();

/*
 * Prints the device identification info of the currently attached devices
 */
void print_dev_ids ();	

/*
 * Prints the catalog (i.e. how many and at which indices devices are currently attached)
 */
void print_catalog ();

/*
 * Prints the params of the specified devices
 */
void print_params (uint32_t devices);

/*
 * Prints the current values of the robot description in a human-readable way
 */
void print_robot_desc ();

/*
 * Prints the current state of the gamepad in a human-readable way
 */
void print_gamepad_state ();

// ******************************************* WRAPPER FUNCTIONS ****************************************** //

/*
 * Call this function from every process that wants to use the shared memory wrapper
 * No return value (will exit on fatal errors).
 */
void shm_init ();

/*
 * Call this function if process no longer wishes to connect to shared memory wrapper
 * No other actions will work after this function is called.
 * No return value.
 */
void shm_stop ();

/*
 * Should only be called from device handler
 * Selects the next available device index and assigns the newly connected device to that location. 
 * Turns on the associated bit in the catalog.
 * Arguments: 
 *    - uin16_t dev_type: the type of the device being connected e.g. LIMITSWITCH, LINEFOLLOWER, etc.
 *    - uint8_t dev_year: year of device manufacture
 *    - uint64_t dev_uid: the unique, random 64-bit uid assigned to the device when flashing
 *    - int *dev_ix: the index that the device was assigned will be put here
 * Returns device index of connected device in dev_ix on success; sets *dev_ix = -1 on failure
 */
void device_connect (uint16_t dev_type, uint8_t dev_year, uint64_t dev_uid, int *dev_ix);

/*
 * Should only be called from device handler
 * Disconnects a device with a given index by turning off the associated bit in the catalog.
 * Arguments
 *    - int dev_ix: index of device in catalog to be disconnected
 * No return value.
 */
void device_disconnect (int dev_ix);

/*	
 * Should be called from every process wanting to read the device data
 * Takes care of updating the param bitmap for fast transfer of commands from executor to device handler
 * Arguments:
 *    - int dev_ix: device index of the device whose data is being requested
 *    - process_t process: the calling process, one of DEV_HANDLER, EXECUTOR, or NET_HANDLER
 *    - stream_t stream: the requested block to read from, one of DATA, COMMAND
 *    - uint32_t params_to_read: bitmap representing which params to be read  (nonexistent params should have corresponding bits set to 0)
 *    - param_val_t params: pointer to array of param_val_t's that is at least as long as highest requested param number
 *            device data will be read into the corresponding param_val_t's
 * Returns 0 on success, -1 on failure (specified device is not connected in shm)
 */
int device_read (int dev_ix, process_t process, stream_t stream, uint32_t params_to_read, param_val_t *params);

/*
 * This function is the exact same as the above function, but instead uses the 64-bit device UID to identify
 * the device that should be read, rather than the device index.
 */
int device_read_uid (uint64_t dev_uid, process_t process, stream_t stream, uint32_t params_to_read, param_val_t *params);

/*	
 * Should be called from every process wanting to write to the device data
 * Takes care of updating the param bitmap for fast transfer of commands from executor to device handler
 * Grabs either one or two semaphores depending on calling process and stream requested.
 * Arguments
 *    - int dev_ix: device index of the device whose data is being written
 *    - process_t process: the calling process, one of DEV_HANDLER, EXECUTOR, or NET_HANDLER
 *    - stream_t stream: the requested block to write to, one of DATA, COMMAND
 *    - uint32_t params_to_read: bitmap representing which params to be written (nonexistent params should have corresponding bits set to 0)
 *    - param_val_t params: pointer to array of param_val_t's that is at least as long as highest requested param number
 *            device data will be written into the corresponding param_val_t's
 * Returns 0 on success, -1 on failure (specified device is not connected in shm)
 */
int device_write (int dev_ix, process_t process, stream_t stream, uint32_t params_to_write, param_val_t *params);

/*
 * This function is the exact same as the above function, but instead uses the 64-bit device UID to identify
 * the device that should be written, rather than the device index.
 */
int device_write_uid (uint64_t dev_uid, process_t process, stream_t stream, uint32_t params_to_write, param_val_t *params);

/*
 * Reads the specified robot description field. Blocks on the robot description semaphore.
 * Arguments:
 *    - field: one of the robot_desc_val_t's defined above to read from
 * Returns one of the robot_desc_val_t's defined in runtime_util that is the current value of the requested field.
 */
robot_desc_val_t robot_desc_read (robot_desc_field_t field);

/*
 * Writes the specified value into the specified field. Blocks on the robot description semaphore.
 * Arguments:
 *    - robot_desc_field_t field: one of the robot_desc_val_t's defined above to write val to
 *    - robot_desc_val_t val: one of the robot_desc_vals defined in runtime_util.c to write to the specified field
 * No return value.
 */
void robot_desc_write (robot_desc_field_t field, robot_desc_val_t val);

/*
 * Reads current state of the gamepad to the provided pointers. 
 * Blocks on both the gamepad semaphore and device description semaphore (to check if gamepad connected).
 * Arguments:
 *    - uint32_t pressed_buttons: pointer to 32-bit bitmap to which the current button bitmap state will be read into
 *    - float *joystick_vals: array of 4 floats to which the current joystick states will be read into
 * Returns 0 on success, -1 on failure (if gamepad is not connected)
 */
int gamepad_read (uint32_t *pressed_buttons, float *joystick_vals);

/*
 * This function writes the given state of the gamepad to shared memory.
 * Blocks on both the gamepad semaphore and device description semaphore (to check if gamepad connected).
 * Arguments:
 *    - uint32_t pressed_buttons: a 32-bit bitmap that corresponds to which buttons are currently pressed
            (only the first 17 bits used, since there are 17 buttons)
 *    - float *joystick_vals: array of 4 floats that contain the values to write to the joystick
 * Returns 0 on success, -1 on failuire (if gamepad is not connected)
 */
int gamepad_write (uint32_t pressed_buttons, float *joystick_vals);

/*
 * Should be called from all processes that want to know current state of the param bitmap (i.e. device handler)
 * Blocks on the param bitmap semaphore for obvious reasons
 * Arguments:
 *    - uint32_t *bitmap: pointer to array of 17 32-bit integers to copy the bitmap into
 * No return value.
 */
void get_param_bitmap (uint32_t *bitmap);

/*
 * Should be called from all processes that want to know device identifiers of all currently connected devices
 * Blocks on catalog semaphore for obvious reasons
 * Arguments:
 *    - dev_id_t *dev_ids: pointer to array of dev_id_t's to copy the information into
 * No return value.
 */
void get_device_identifiers (dev_id_t *dev_ids);

/*
 * Should be called from all processes that want to know which dev_ix's are valid
 * Blocks on catalog semaphore for obvious reasons
 * Arguments:
 *    - catalog: pointer to 32-bit integer into which the current catalog will be read into
 * No return value.
 */
void get_catalog (uint32_t *catalog);

#endif