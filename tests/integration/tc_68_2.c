#include "../test.h"

char check_output_1[] = "Autonomous setup has begun!\n";

char check_output_2[] = "autonomous printing again\n";

char check_output_3[] = "\tRUN_MODE = AUTO\n";

char check_output_4[] = "\tRUN_MODE = IDLE\n";

char check_output_5[] =
    "Traceback (most recent call last):\n";

//we have to skip the File: <file path> because on the pi it's /home/pi/c-runtime
//but on Travis it's /root/pi/c-runtime
char check_output_6[] =
    "line 25, in teleop_main\n"
    "    oops = 1 / 0\n"
    "ZeroDivisionError: division by zero\n";

char check_output_7[] = "Python function teleop_main call failed\n";

char check_output_8[] = "\tRUN_MODE = TELEOP\n";

char check_output_9[] =
	"Challenge 0 result: 9302\n"
	"Challenge 1 result: [2, 661, 35963]";

char check_output_10[] = "Suppressing output: too many messages...";

int main ()
{
	//set everything up
	start_test("executor sanity test");
	start_shm();
	start_net_handler();
	start_executor("executor_sanity", "executor_sanity");

	//poke the system
	//this section checks the autonomous code (should generate some print statements)
	send_start_pos(SHEPHERD, RIGHT);
	send_run_mode(SHEPHERD, AUTO);
	sleep(1);
	print_shm();
	sleep(2);
	send_run_mode(SHEPHERD, IDLE);
	print_shm();

	//this section checks the teleop code (should generate division by zero error)
	send_run_mode(DAWN, TELEOP);
	print_shm();
	send_run_mode(DAWN, IDLE);
	print_shm();

	//this section runs the coding challenges (should not error or time out)
	char *inputs[] = { "2039", "190172344" };
	send_challenge_data(DAWN, inputs, 2);

	//stop all the processes
	stop_executor();
	stop_net_handler();
	stop_shm();
	end_test();

	//check outputs
	in_rest_of_output(check_output_1);
	in_rest_of_output(check_output_2);
	in_rest_of_output(check_output_3);
	in_rest_of_output(check_output_2);
	in_rest_of_output(check_output_4);
	in_rest_of_output(check_output_5);
	in_rest_of_output(check_output_6);
	in_rest_of_output(check_output_7);
	in_rest_of_output(check_output_8);
	in_rest_of_output(check_output_9);
	not_in_output(check_output_10); //check to make sure we don't get the suppressing messages bug

	return 0;
}