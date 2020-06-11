#define PY_SSIZE_T_CLEAN
#include <python3.6/Python.h>              // For Python's C API
#include <stdio.h>                         //for i/o
#include <stdlib.h>                        //for standard utility functions (exit, sleep)
#include <sys/types.h>                     //for sem_t and other standard system types
#include <stdint.h>                        //for standard int types
#include <unistd.h>                        //for sleep
#include <pthread.h>                       //for POSIX threads
#include <signal.h>                        // Used to handle SIGTERM, SIGINT, SIGKILL
#include <time.h>                          // for getting time
#include "../runtime_util/runtime_util.h"  //for runtime constants (TODO: consider removing relative pathname in include)
#include "../shm_wrapper/shm_wrapper.h"    // Shared memory wrapper to get/send device data
#include "../shm_wrapper_aux/shm_wrapper_aux.h" // Shared memory wrapper for robot state
#include "../logger/logger.h"              // for runtime logger


// Global variables to all functions and threads
const char* loader_file = "code_loader.py";
const char* api_module = "studentapi";
char* student_module;
PyObject *pModule, *pAPI, *pPrint, *pRobot, *pGamepad;
pthread_t mode_thread;
pthread_t handler_thread;

// Timings for all modes
const struct timespec setup_time = {3, 0};    // Max time allowed for setup functions
#define freq 10 // How many times per second the main loop will run
const struct timespec main_interval = {0, (long) (1.0/freq * 1e9)};

/**
 *  Struct used as input to all threads. Not all threads will use all arguments.
 */
typedef struct thread_args {
    char* func_name;
    pthread_cond_t* cond;
    char* mode;
    struct timespec* timeout;
} thread_args_t;


/**
 *  Returns the appropriate string representation from the given mode.
 */
const char* get_mode_str(robot_desc_val_t mode) {
    if (mode == AUTO) {
        return "autonomous";
    }
    else if (mode == TELEOP) {
        return "teleop";
    }
    else if (mode == IDLE) {
        return "idle";
    }
    return NULL;
}


/**
 *  Cleanup function for threads that cleans up Python objects. 
 * 
 *  Inputs:
 *      args: a PyObject* that needs their reference decremented
 */
void py_function_cleanup(void* args) {
    PyObject** objects = (PyObject**) args;
    Py_DECREF(objects[0]);
    Py_XDECREF(objects[1]);
}


/**
 *  Closes all executor processes and exits cleanly.
 */
void executor_stop() {
	printf("\nShutting down executor...\n");
    pthread_cancel(handler_thread);
    log_runtime(DEBUG, "Commencing handler cancel");
    pthread_join(handler_thread, NULL);
    log_runtime(DEBUG, "Cancelled handler thread");
    pthread_cancel(mode_thread);
    pthread_join(mode_thread, NULL);
    log_runtime(DEBUG, "joining to make sure thread is cancelled");
    Py_XDECREF(pAPI);
    Py_XDECREF(pPrint);
    Py_XDECREF(pRobot);
    Py_XDECREF(pGamepad);
    Py_XDECREF(pModule);
    // Py_FinalizeEx();
    log_runtime(DEBUG, "Stopping aux shm");
    shm_aux_stop(EXECUTOR);
    log_runtime(DEBUG, "Aux SHM stopped");
    logger_stop();
	exit(1);
}


/**
 *  Handler for keyboard interrupts SIGINT (Ctrl + C)
 */
void sigintHandler(int signum) {
    executor_stop();
}


/**
 *  Initializes the executor process. Must be the first thing called.
 *  Input: 
 *      student_code: string representing the name of the student's Python file, without the .py
 */
void executor_init(char* student_code) {
    logger_init(EXECUTOR);
    shm_aux_init(EXECUTOR);
    log_runtime(DEBUG, "Aux SHM initialized");
    student_module = student_code;
    Py_Initialize();
    PyRun_SimpleString("import sys;sys.path.insert(0, '.')");

    pAPI = PyImport_ImportModule(api_module);
    if (pAPI == NULL) {
        PyErr_Print();
        log_runtime(ERROR, "Could not import API module");
        executor_stop();
    }

    pPrint = PyObject_GetAttrString(pAPI, "_print");
    if (pPrint == NULL) {
        PyErr_Print();
        log_runtime(ERROR, "Could not find _print");
        executor_stop();
    }

    PyObject* robot_class = PyObject_GetAttrString(pAPI, "Robot");
    if (robot_class == NULL) {
        PyErr_Print();
        log_runtime(ERROR, "Could not find Robot class");
        executor_stop();
    }
    pRobot = PyObject_CallObject(robot_class, NULL);
    if (pRobot == NULL) {
        PyErr_Print();
        log_runtime(ERROR, "Could not instantiate Robot");
        executor_stop();
    }
    Py_DECREF(robot_class);

    PyObject* gamepad_class = PyObject_GetAttrString(pAPI, "Gamepad");
    if (gamepad_class == NULL) {
        PyErr_Print();
        log_runtime(ERROR, "Could not find Gamepad class");
        executor_stop();
    }
    log_runtime(DEBUG, "Reading mode"); 
    char* mode_str = get_mode_str(robot_desc_read(RUN_MODE));
    log_runtime(DEBUG, "Got mode from robot desc");
    pGamepad = PyObject_CallFunction(gamepad_class, "s", mode_str);
    if (pGamepad == NULL) {
        PyErr_Print();
        log_runtime(ERROR, "Could not instantiate Gamepad");
        executor_stop();
    }
    Py_DECREF(gamepad_class);

    pModule = PyImport_ImportModule(student_module);
    if (pModule == NULL) {
        PyErr_Print();
        char msg[64];
        sprintf(msg, "Could not import module: %s", student_module);
        log_runtime(ERROR, msg);
        executor_stop();
    }
    
    int flag = PyObject_SetAttrString(pModule, "print", pPrint);
    flag |= PyObject_SetAttrString(pModule, "Robot", pRobot);
    flag |= PyObject_SetAttrString(pModule, "Gamepad", pGamepad);
    if (flag != 0) {
        PyErr_Print();
        log_runtime(ERROR, "Could not insert API into student code.");
        executor_stop();
    }
}


/**
 *  Runs the Python function specified in the arguments.
 * 
 *  Behavior: This is a blocking function  and will block the calling thread for the length of
 *  the Python function call. This function can be called standalone or run as a separate thread.
 * 
 *  Inputs: Uses thread_args_t as input struct.
 *      Necessary fields:
 *          func_name: string of function name to run in the student code
 *          mode: string of the current mode
 *      Optional fields:
 *          cond: condition variable that blocks the calling thread, used with `run_function_timeout`
 */
void* run_py_function(void* thread_args) {
    thread_args_t* args = (thread_args_t*) thread_args;

    PyObject* pMode = PyUnicode_FromString(args->mode);
    int err = PyObject_SetAttrString(pGamepad, "mode", pMode);
    if (err != 0) {
        char msg[100];
        sprintf(msg, "Couldn't assign mode for Gamepad while trying to run %s", args->func_name);
        log_runtime(ERROR, msg);
        pthread_cond_signal(args->cond);
        return NULL;
    }
    Py_DECREF(pMode);

    PyObject* pFunc = PyObject_GetAttrString(pModule, args->func_name);
    PyObject* pValue;

    if (pFunc && PyCallable_Check(pFunc)) {
        PyObject* objects[2] = {pFunc, NULL};
        pthread_cleanup_push(py_function_cleanup, (void*) objects);
        pValue = PyObject_CallObject(pFunc, NULL);
        pthread_cleanup_pop(1);
        if (pValue == NULL) {
            PyErr_Print();
            char msg[64];
            sprintf(msg, "Python function %s call failed", args->func_name);
            log_runtime(ERROR, msg);
        }
        else {
            Py_DECREF(pValue);
        }
    }
    else {
        if (PyErr_Occurred()) {
            PyErr_Print();
        }
        char msg[64];
        sprintf(msg, "Cannot find function in student code: %s\n", args->func_name);
        log_runtime(ERROR, msg);
    }
    // Send the signal that the thread is done, using the condition variable.
    if (args->cond != NULL) {
        pthread_cond_signal(args->cond);
    }
    return NULL;
}


/**
 *  Runs the specified Python function in another thread with the specified timeout.
 *  Uses `run_py_function` as the underlying thread. Meant to be used for the setup functions.
 * 
 *  Behavior: This is a blocking function and will block the calling thread for up to the length of the timeout.
 *  This function can be called standalone or run as a separate thread.
 * 
 *  Inputs: Uses thread_args_t as input struct. Fields necessary are:
 *      func_name: string of function name to run in the student code
 *      mode: string of the current mode
 *      timeout: timespec of the max timeout the function should run
 */
void run_function_timeout(thread_args_t* args) {
    // thread_args_t* args = (thread_args_t*) thread_args;
    pthread_t tid;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    args->cond = &cond;
    struct timespec time;
    pthread_mutex_lock(&mutex);
    clock_gettime(CLOCK_REALTIME, &time);
    time.tv_sec += args->timeout->tv_sec;
    time.tv_nsec += args->timeout->tv_nsec;
    pthread_create(&tid, NULL, run_py_function, (void*) args);
    int err = pthread_cond_timedwait(&cond, &mutex, &time);
    if (err == ETIMEDOUT) {
        char buffer[128];
        sprintf(buffer, "Function %s is taking longer than %lu seconds and was timed out.", args->func_name, args->timeout->tv_sec);
        log_runtime(WARN, buffer);
    }
    else if (err != 0) {
        char msg[100];
        sprintf(msg, "Function %s failed with error code %d", args->func_name, err);
        log_runtime(ERROR, msg);
    }
    pthread_mutex_unlock(&mutex);
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);
    pthread_cancel(tid);
    pthread_join(tid, NULL);
}


/**
 *  Runs the given Python function in a while loop. Meant to be used for the main functions.
 * 
 *  Behavior: This is a blocking function and will block the calling thread forever.
 *  This should only be run inside a separate thread.
 * 
 *  Inputs: Uses thread_args_t as input struct. Fields necessary are:
 *      func_name: string of function name to run in the student code
 *      mode: string of the current 
 *      timeout: maximum time for one loop to send a warning
 */
void run_function_loop(thread_args_t* args) {
    // thread_args_t* args = (thread_args_t*) thread_args;
    // int err = pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    // if (err != 0) {
    //     char msg[100];
    //     sprintf(msg, "Cannot set CANCEL_ASYNC for run_py_function thread for %s", args->func_name);
    //     log_runtime(WARN, msg);
    // }

    PyObject* pMode = PyUnicode_FromString(args->mode);
    int err = PyObject_SetAttrString(pGamepad, "mode", pMode);
    if (err != 0) {
        char msg[100];
        sprintf(msg, "Couldn't assign mode for Gamepad while trying to run %s", args->func_name);
        log_runtime(ERROR, msg);
        pthread_cond_signal(args->cond);
    }
    Py_DECREF(pMode);

    PyObject* pFunc = PyObject_GetAttrString(pModule, args->func_name);
    PyObject* pValue = NULL;

    if (pFunc && PyCallable_Check(pFunc)) {
        PyObject* objects[2] = {pFunc, pValue};
        pthread_cleanup_push(py_function_cleanup, (void*) objects);
        struct timespec start, end;
        long time;
        while (1) {
            clock_gettime(CLOCK_MONOTONIC, &start);
            pValue = PyObject_CallObject(pFunc, NULL);
            clock_gettime(CLOCK_MONOTONIC, &end);
            time = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
            if (time > args->timeout->tv_nsec) {
                char buffer[128];
                sprintf(buffer, "Function %s is taking longer than %lu milliseconds, indicating a loop in the code.", args->func_name, (long) (args->timeout->tv_nsec / 1e6));
                log_runtime(WARN, buffer);
            }
            if (pValue == NULL) {
                PyErr_Print();
                char msg[64];
                sprintf(msg, "Python function %s call failed", args->func_name);
                log_runtime(ERROR, msg);
            } 
            else {
                Py_DECREF(pValue);
            }
        }
        pthread_cleanup_pop(1);
    }
    else {
        if (PyErr_Occurred())
            PyErr_Print();
        char msg[64];
        sprintf(msg, "Cannot find function in student code: %s\n", args->func_name);
        log_runtime(ERROR, msg);
    }
}


/**
 *  Begins the given game mode and calls setup and main appropriately. Will run main forever.
 * 
 *  Behavior: This is a blocking function and will block the calling thread forever.
 *  This should only be run as a separate thread.
 * 
 *  Inputs:
 *      args: string of the mode to start running
 */
void* run_mode_functions(void* args) {
    char* mode = (char*) args;
    char setup_str[20];
    char main_str[20];
    sprintf(setup_str, "%s_setup", mode);
    sprintf(main_str, "%s_main", mode);
    thread_args_t setup_args = {setup_str, NULL, mode, &setup_time};
    run_function_timeout(&setup_args);
    log_runtime(DEBUG, "Finished setup");
    thread_args_t main_args = {main_str, NULL, mode, &main_interval};
    run_function_loop(&main_args);
    return NULL;
}


/**
 *  Bootloader that calls `run_mode_functions` with the correct mode. Ensures any previously running
 *  thread is terminated first.
 * 
 *  Behavior: This is a nonblocking function and will begin running the current mode forever.
 */
void* handle_mode_changes(void* args) {
    robot_desc_val_t mode = IDLE;
    robot_desc_val_t new_mode;

    FILE* garbage = fopen("/dev/null", "w");
    do {
        new_mode = robot_desc_read(RUN_MODE);
        fprintf(garbage, "test"); // Really weird way to create a cancellation point
        if (new_mode != mode) {
            mode = new_mode;
            pthread_cancel(mode_thread);
            pthread_join(mode_thread, NULL);
            if (mode != IDLE) {
                pthread_create(&mode_thread, NULL, run_mode_functions, (void*) get_mode_str(mode));
            }
        }
    } while (1);
}


///////////////////////////////////// OLD AND UNUSED CODE


/**
 *  Runs any student's Python file using the `code_loader` for a specific mode.
 * 
 *  Behavior: This is a blocking function and will block the thread for up to the length of the mode.
 *  This can be run as a separate thread or standalone.
 * 
 *  Inputs:
 *      args: string representing the mode to run
 */
void* run_student_file(void* args) {
    /* allow the thread to be killed at any time */
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    char* mode = (char*) args;
    FILE* file = fopen(loader_file, "r");
    wchar_t* py_args[3] = {
        Py_DecodeLocale("code_loader", NULL),
        Py_DecodeLocale(student_module, NULL),
        Py_DecodeLocale(mode, NULL)
    };
    PySys_SetArgvEx(3, py_args, 0);
    PyRun_SimpleFile(file, loader_file);
    return NULL;
}


/**
 *  Spawns a new subprocess that runs the `code_loader` on the student's code for a specified mode.
 * 
 *  Behavior: This is a blocking function and blocks for the length of the mode.
 * 
 *  Inputs:
 *      args: string representing the mode to run
 */
void* run_file_subprocess(void* args) {
    char command[256];
    char* mode = (char*) args;
    sprintf(command, "python3.6 %s %s %s", loader_file, student_module, mode);
    FILE* process = popen(command, "r");
    if (process == NULL) {
        log_runtime(ERROR, "Failed to start process for");
        log_runtime(ERROR, command);
    }
    // For debugging only
    while (fgets(command, 256, process) != NULL) {
        printf("%s", command);
    }
    int status;
    if ((status = pclose(process)) != 0) {
        log_runtime(ERROR, "Error occurred while calling process");
        log_runtime(ERROR, command);
    }
    return NULL;
}


/**
 *  Bootloader that runs `run_file_subprocess` in a separate thread.
 * 
 *  Behavior: This is a nonblocking function.
 * 
 *  Inputs:
 *      mode: enum representing the mode to run
 */
void start_loader_subprocess(robot_desc_val_t mode) {
    pthread_t tid;
    char* mode_str;
    if (mode == AUTO) {
        mode_str = "autonomous";
    }
    else if (mode == TELEOP) {
        mode_str = "teleop";
    }
    pthread_create(&tid, NULL, run_file_subprocess, (void*) mode_str);
    pthread_detach(tid);
}

/////////////////////////////////////////////////

int main(int argc, char* argv[]) {
    signal(SIGINT, sigintHandler);
    // signal(SIGALRM, sigalrmHandler);
    executor_init("studentcode");
    // struct timespec start, end;
    // clock_gettime(CLOCK_MONOTONIC, &start);
    // run_student_file("autonomous");
    // clock_gettime(CLOCK_MONOTONIC, &end);
    // printf("Elapsed milliseconds for file: %lu \n", (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000);

    // clock_gettime(CLOCK_MONOTONIC, &start);
    // run_file_subprocess(AUTO);
    // clock_gettime(CLOCK_MONOTONIC, &end);
    // printf("Elapsed milliseconds for subprocess: %lu \n", (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000);    

    //start_loader_thread(AUTO);
    //start_loader_subprocess(AUTO);

    // struct timespec diff = {2, 0};
    // thread_args_t args = {"autonomous_setup", NULL, "autonomous", &diff};
    // pthread_t id;
    // pthread_create(&id, NULL, run_function_timeout, (void*) &args);
    // // pthread_join(id, NULL);
    // thread_args_t args1 = {"set_motor", NULL, "autonomous", &diff};
    // pthread_create(&id, NULL, run_function_timeout, (void*) &args1);

    // sleep(1);
    // student_module = "test_studentapi";
    // run_py_function("test_api");

    // start_mode_thread(AUTO, &mode_thread);

    pthread_create(&handler_thread, NULL, handle_mode_changes, NULL);
    // pthread_join(handler_thread, NULL);
    long long i = 0;
    while(1) {
        if ((i % 100000000) == 0) {
            // log_runtime(DEBUG, "Main function iteration");
        }
        i++;
    }
    return 0;
}
