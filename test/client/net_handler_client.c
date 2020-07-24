#include "net_handler_client.h"

//throughout this code, net_handler is abbreviated "nh"
pid_t nh_pid;                     //holds the pid of the net_handler process
struct sockaddr_in udp_servaddr;  //holds the udp server address
pthread_t dump_tid;               //holds the thread id of the output dumper threads
pthread_mutex_t print_udp_mutex;  //lock over whether to print the next received udp
int print_next_udp;               //0 if we want to suppress incoming dev data, 1 to print incoming dev data to stdout

int nh_tcp_shep_fd = -1;          //holds file descriptor for TCP Shepherd socket
int nh_tcp_dawn_fd = -1;          //holds file descriptor for TCP Dawn socket
int nh_udp_fd = -1;               //holds file descriptor for UDP Dawn socket
FILE *tcp_output_fp = NULL;       //holds current output location of incoming TCP messages
FILE *udp_output_fp = NULL;       //holds current output location of incoming UDP messages
FILE *null_fp = NULL;             //file pointer to /dev/null

// ************************************* HELPER FUNCTIONS ************************************** //

// Returns the number of milliseconds since the Unix Epoch
static uint64_t millis() {
	struct timeval time; // Holds the current time in seconds + microsecondsx
	gettimeofday(&time, NULL);
	uint64_t s1 = (uint64_t)(time.tv_sec) * 1000;  // Convert seconds to milliseconds
	uint64_t s2 = (time.tv_usec / 1000);		   // Convert microseconds to milliseconds
	return s1 + s2;
}

static int connect_tcp (int client)
{
	struct sockaddr_in serv_addr, cli_addr;
	int sockfd;
	if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		printf("socket: failed to create listening socket: %s\n", strerror(errno));
		stop_net_handler();
		exit(1);
	}

	int optval = 1;
	if ((setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(int))) != 0) {
		printf("setsockopt: failed to set listening socket for reuse of port: %s\n", strerror(errno));
	}
	
	//set the elements of cli_addr
	memset(&cli_addr, '\0', sizeof(struct sockaddr_in));     //initialize everything to 0
	cli_addr.sin_family = AF_INET;                           //use IPv4
	cli_addr.sin_port = (client == SHEPHERD_CLIENT) ? htons(SHEPHERD_PORT) : htons(DAWN_PORT); //use specifid port to connect
	cli_addr.sin_addr.s_addr = htonl(INADDR_ANY);            //use any address set on this machine to connect
	
	//bind the client side too, so that net_handler can verify it's the proper client
	if ((bind(sockfd, (struct sockaddr *)&cli_addr, sizeof(struct sockaddr_in))) != 0) {
		printf("bind: failed to bind listening socket to client port: %s\n", strerror(errno));
		close(sockfd);
		stop_net_handler();
		exit(1);
	}
	
	//set the elements of serv_addr
	memset(&serv_addr, '\0', sizeof(struct sockaddr_in));     //initialize everything to 0
	serv_addr.sin_family = AF_INET;                           //use IPv4
	serv_addr.sin_port = htons(RASPI_PORT);                   //want to connect to raspi port
	serv_addr.sin_addr.s_addr = inet_addr(RASPI_ADDR);
	
	//connect to the server
	if (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(struct sockaddr_in)) != 0) {
		printf("connect: failed to connect to socket: %s\n", strerror(errno));
		close(sockfd);
		stop_net_handler();
		exit(1);
	}
	
	//send the verification byte
	writen(sockfd, &client, 1);
	
	return sockfd;
}

static void recv_udp_data (int udp_fd)
{
	int max_size = 4096;
	uint8_t msg[max_size];
	struct sockaddr_in recvaddr;
	socklen_t addrlen = sizeof(recvaddr);
	int recv_size;
	
	//receive message from udp socket
	if ((recv_size = recvfrom(udp_fd, msg, max_size, 0, (struct sockaddr*) &recvaddr, &addrlen)) < 0) {
		fprintf(udp_output_fp, "recvfrom: %s\n", strerror(errno));
	}
	fprintf(udp_output_fp, "Raspi IP is %s:%d\n", inet_ntoa(recvaddr.sin_addr), ntohs(recvaddr.sin_port));
	fprintf(udp_output_fp, "Received data size %d\n", recv_size);
	DevData* dev_data = dev_data__unpack(NULL, recv_size, msg);
	if (dev_data == NULL) {
		printf("Error unpacking incoming message\n");
	}
	
	// display the message's fields.
	fprintf(udp_output_fp, "Received:\n");
	for (int i = 0; i < dev_data->n_devices; i++) {
		fprintf(udp_output_fp, "Device No. %d: ", i);
		fprintf(udp_output_fp, "\ttype = %s, uid = %llu, itype = %d\n", dev_data->devices[i]->name, dev_data->devices[i]->uid, dev_data->devices[i]->type);
		fprintf(udp_output_fp, "\tParams:\n");
		for (int j = 0; j < dev_data->devices[i]->n_params; j++) {
			fprintf(udp_output_fp, "\t\tparam \"%s\" has type ", dev_data->devices[i]->params[j]->name);
			switch (dev_data->devices[i]->params[j]->val_case) {
				case (PARAM__VAL_FVAL):
					fprintf(udp_output_fp, "FLOAT with value %f\n", dev_data->devices[i]->params[j]->fval);
					break;
				case (PARAM__VAL_IVAL):
					fprintf(udp_output_fp, "INT with value %d\n", dev_data->devices[i]->params[j]->ival);
					break;
				case (PARAM__VAL_BVAL):
					fprintf(udp_output_fp, "BOOL with value %d\n", dev_data->devices[i]->params[j]->bval);
					break;
				default:
					fprintf(udp_output_fp, "ERROR: no param value");
					break;
			}
		}
	}
	
	// Free the unpacked message
	dev_data__free_unpacked(dev_data, NULL);
	fflush(udp_output_fp);
	
	//if we were asked to print the last UDP message, set output back to /dev/null
	pthread_mutex_lock(&print_udp_mutex);
	if (print_next_udp) {
		print_next_udp = 0;
		udp_output_fp = null_fp;
	}
	pthread_mutex_unlock(&print_udp_mutex);
}

static int recv_tcp_data (int client, int tcp_fd)
{
	//variables to read messages into
	Text* msg;
	net_msg_t msg_type;
	uint8_t *buf;
	uint16_t len;
	char client_str[16];
	if (client == SHEPHERD_CLIENT) {
		strcpy(client_str, "SHEPHERD");
	} else {
		strcpy(client_str, "DAWN");
	}

	fprintf(tcp_output_fp, "From %s:\n", client_str);
	//parse message
	if (parse_msg(tcp_fd, &msg_type, &len, &buf) == 0) {
		printf("Net handler disconnected\n");
		return -1;
	}
	
	//unpack the message
	if ((msg = text__unpack(NULL, len, buf)) == NULL) {
		fprintf(tcp_output_fp, "Error unpacking incoming message from %s\n", client_str);
	}
	
	//print the incoming message
	if (msg_type == LOG_MSG) {
		for (int i = 0; i < msg->n_payload; i++) {
			fprintf(tcp_output_fp, "%s", msg->payload[i]);
		}
	} else if (msg_type == CHALLENGE_DATA_MSG) {
		for (int i = 0; i < msg->n_payload; i++) {
			fprintf(tcp_output_fp, "Challenge %d result: %s\n", i, msg->payload[i]);
		}
	}
	fflush(tcp_output_fp);
	
	//free allocated memory
	free(buf);
	text__free_unpacked(msg, NULL);
	
	return 0;
}

//dumps output from net handler stdout to this process's standard out
static void *output_dump (void *args)
{
	const int sample_size = 10; //number of messages that need to come in before disabling output
	const uint64_t disable_threshold = 50; //if the interval between each of the past sample_size messages has been less than this many milliseconds, disable output
	const uint64_t enable_threshold = 1000; //if this many milliseconds have passed between now and last received message, enable output
	uint64_t last_received_time = 0, curr_time;
	uint32_t less_than_disable_thresh = 0;
	
	fd_set read_set;
	int maxfd = (nh_tcp_dawn_fd > nh_tcp_shep_fd) ? nh_tcp_dawn_fd : nh_tcp_shep_fd;
	maxfd = (nh_udp_fd > maxfd) ? nh_udp_fd : maxfd;
	maxfd++;
	
	//wait for net handler to create some output, then print that output to stdout of this process
	while (1) {
		//set up the read_set argument to selct()
		FD_ZERO(&read_set);
		FD_SET(nh_tcp_shep_fd, &read_set);
		FD_SET(nh_tcp_dawn_fd, &read_set);
		FD_SET(nh_udp_fd, &read_set);
		
		//prepare to accept cancellation requests over the select
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		
		//wait for something to happen
		if (select(maxfd, &read_set, NULL, NULL, NULL) < 0) {
			printf("select: output dump: %s\n", strerror(errno));
		}
		
		//deny all cancellation requests until the next loop
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		
		//enable tcp output if more than enable_thresh has passed between last time and previous time
		if (FD_ISSET(nh_tcp_shep_fd, &read_set) || FD_ISSET(nh_tcp_dawn_fd, &read_set)) {
			curr_time = millis();
			if (curr_time - last_received_time >= enable_threshold) {
				less_than_disable_thresh = 0;
				tcp_output_fp = stdout;
			}
			if (curr_time - last_received_time <= disable_threshold) {
				less_than_disable_thresh++;
				if (less_than_disable_thresh == sample_size) {
					printf("Suppressing output: too many messages...\n\n");
					fflush(stdout);
					tcp_output_fp = null_fp;
				}
			}
			last_received_time = curr_time;
		}
		
		//if we were asked to print the next UDP, set the UDP pointer
		pthread_mutex_lock(&print_udp_mutex);
		if (print_next_udp) {
			udp_output_fp = stdout;
		}
		pthread_mutex_unlock(&print_udp_mutex);

		//print stuff from whichever file descriptors are ready for reading...
		if (FD_ISSET(nh_tcp_shep_fd, &read_set)) {
			if (recv_tcp_data(SHEPHERD, nh_tcp_shep_fd) == -1) {
				return NULL;
			}
		}
		if (FD_ISSET(nh_tcp_dawn_fd, &read_set)) {
			if (recv_tcp_data(DAWN, nh_tcp_dawn_fd) == -1) {
				return NULL;
			}
		}
		if (FD_ISSET(nh_udp_fd, &read_set)) {
			recv_udp_data(nh_udp_fd);
		}
	}
	return NULL;
}

// ************************************* NET HANDLER CLIENT FUNCTIONS ************************** //

void start_net_handler ()
{

	//fork net_handler process
	if ((nh_pid = fork()) < 0) {
		printf("fork: %s\n", strerror(errno));
	} else if (nh_pid == 0) { //child
		//exec the actual net_handler process
		if (execlp("../../net_handler/net_handler", "net_handler", (char *) 0) < 0) {
			printf("execlp: %s\n", strerror(errno));
		}
	} else { //parent
		sleep(1); //allows net_handler to set itself up

		//Connect to the raspi networking ports to catch network output
		nh_tcp_dawn_fd = connect_tcp(DAWN_CLIENT);
		nh_tcp_shep_fd = connect_tcp(SHEPHERD_CLIENT);
		if ((nh_udp_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
			printf("socket: UDP socket creation failed...\n");
			stop_net_handler();
			exit(1);
		}
		memset(&udp_servaddr, 0, sizeof(struct sockaddr_in));
		udp_servaddr.sin_family = AF_INET; 
		udp_servaddr.sin_addr.s_addr = inet_addr(RASPI_ADDR); 
		udp_servaddr.sin_port = htons(RASPI_UDP_PORT); 
		
		//open /dev/null
		null_fp = fopen("/dev/null", "w");

		//init the mutex that will control whether udp prints to screen
		if (pthread_mutex_init(&print_udp_mutex, NULL) != 0) {
			printf("pthread_mutex_init: print udp mutex\n");
		}
		print_next_udp = 0;
		udp_output_fp = null_fp; //by default set to output to /dev/null

		//start the thread that is dumping output from net_handler to stdout of this process
		if (pthread_create(&dump_tid, NULL, output_dump, NULL) != 0) {
			printf("pthread_create: output dump\n");
		}
		sleep(1); //allow time for thread to dump output before returning to client
	}
}

void stop_net_handler ()
{
	//send signal to net_handler and wait for termination
	if (kill(nh_pid, SIGINT) < 0) {
		printf("kill: %s\n", strerror(errno));
	}
	if (waitpid(nh_pid, NULL, 0) < 0) {
		printf("waitpid: %s\n", strerror(errno));
	}
	
	//killing net handler should cause dump thread to return, so join with it
	if (pthread_join(dump_tid, NULL) != 0) {
		printf("pthread_join: output dump\n");
	}
	
	//close all the file descriptors
	if (nh_tcp_shep_fd != -1) {
		close(nh_tcp_shep_fd);
	}
	if (nh_tcp_dawn_fd != -1) {
		close(nh_tcp_dawn_fd);
	}
	if (nh_udp_fd != -1) {
		close(nh_udp_fd);
	}
}	
	

void send_run_mode (int client, int mode)
{
	RunMode run_mode = RUN_MODE__INIT;
	uint8_t *send_buf;
	uint16_t len;
	
	//set the right mode
	switch (mode) {
		case (IDLE_MODE):
			run_mode.mode = MODE__IDLE;
			break;
		case (AUTO_MODE):
			run_mode.mode = MODE__AUTO;
			break;
		case (TELEOP_MODE):
			run_mode.mode = MODE__TELEOP;
			break;
		default:
			printf("ERROR: sending run mode message\n");
	}
	
	//build the message
	len = run_mode__get_packed_size(&run_mode);
	send_buf = make_buf(RUN_MODE_MSG, len);
	run_mode__pack(&run_mode, send_buf + 3);
	
	//send the message
	if (client == SHEPHERD_CLIENT) {
		writen(nh_tcp_shep_fd, send_buf, len + 3);
	} else {
		writen(nh_tcp_dawn_fd, send_buf, len + 3);
	}
	free(send_buf);
	sleep(1); //allow time for net handler and runtime to react and generate output before returning to client
}

void send_start_pos (int client, int pos)
{
	StartPos start_pos = START_POS__INIT;
	uint8_t *send_buf;
	uint16_t len;
	
	//set the right mode
	switch (pos) {
		case (LEFT_POS):
			start_pos.pos = POS__LEFT;
			break;
		case (RIGHT_POS):
			start_pos.pos = POS__RIGHT;
			break;
		default:
			printf("ERROR: sending run mode message\n");
	}
	
	//build the message
	len = start_pos__get_packed_size(&start_pos);
	send_buf = make_buf(START_POS_MSG, len);
	start_pos__pack(&start_pos, send_buf + 3);
	
	//send the message
	if (client == SHEPHERD_CLIENT) {
		writen(nh_tcp_shep_fd, send_buf, len + 3);
	} else {
		writen(nh_tcp_dawn_fd, send_buf, len + 3);
	}
	free(send_buf);
	sleep(1); //allow time for net handler and runtime to react and generate output before returning to client
}

void send_gamepad_state (uint32_t buttons, float joystick_vals[4])
{
	GpState gp_state = GP_STATE__INIT;
	uint8_t *send_buf;
	uint16_t len;
	
	//build the message
	gp_state.connected = 1;
	gp_state.buttons = buttons;
	gp_state.n_axes = 4;
	gp_state.axes = malloc(sizeof(double) * 4);
	for (int i = 0; i < 4; i++) {
		gp_state.axes[i] = joystick_vals[i];
	}
	len = gp_state__get_packed_size(&gp_state);
	send_buf = malloc(len);
	gp_state__pack(&gp_state, send_buf);
	
	//send the message
	sendto(nh_udp_fd, send_buf, len, 0, (struct sockaddr *)&udp_servaddr, sizeof(struct sockaddr_in));
	
	//free everything
	free(gp_state.axes);
	free(send_buf);
	sleep(1); //allow time for net handler and runtime to react and generate output before returning to client (executor timeout is currently set to 5 seconds)
}

void send_challenge_data (int client, char **data)
{
	Text challenge_data = TEXT__INIT;
	uint8_t *send_buf;
	uint16_t len;
	
	//build the message
	challenge_data.payload = malloc(sizeof(char *) * NUM_CHALLENGES);
	challenge_data.n_payload = NUM_CHALLENGES;
	for (int i = 0; i < NUM_CHALLENGES; i++) {
		len = strlen(data[i]);
		challenge_data.payload[i] = malloc(sizeof(char) * len);
		strcpy(challenge_data.payload[i], data[i]);
	}
	len = text__get_packed_size(&challenge_data);
	send_buf = make_buf(CHALLENGE_DATA_MSG, len);
	text__pack(&challenge_data, send_buf + 3);
	
	//send the message
	if (client == SHEPHERD_CLIENT) {
		writen(nh_tcp_shep_fd, send_buf, len + 3);
	} else {
		writen(nh_tcp_dawn_fd, send_buf, len + 3);
	}
	
	//free everything
	for (int i = 0; i < NUM_CHALLENGES; i++) {
		free(challenge_data.payload[i]);
	}
	free(challenge_data.payload);
	free(send_buf);
	sleep(6); //allow time for net handler and runtime to react and generate output before returning to client (executor timeout is currently set to 5 seconds)
}

void send_device_data (dev_data_t *data, int num_devices)
{
	DevData dev_data = DEV_DATA__INIT;
	uint8_t *send_buf;
	uint16_t len;
	
	//build the message
	device_t *curr_device;
	uint8_t curr_type;
	dev_data.n_devices = num_devices;
	dev_data.devices = malloc(sizeof(Device *) * num_devices);
	
	//set each device
	for (int i = 0; i < num_devices; i++) {
		if ((curr_type = device_name_to_type(data[i].name)) == (uint8_t) -1) {
			printf("ERROR: no such device \"%s\"\n", data[i].name);
		}
		//fill in device fields
		curr_device = get_device(curr_type);
		Device dev = DEVICE__INIT;
		dev.name = curr_device->name;
		dev.uid = data[i].uid;
		dev.type = curr_type;
		dev.n_params = curr_device->num_params;
		dev.params = malloc(sizeof(Param *) * curr_device->num_params);
		
		//set each param
		for (int j = 0; j < curr_device->num_params; j++) {
			//fill in param fields
			Param prm = PARAM__INIT;
			prm.val_case = PARAM__VAL_BVAL;
			prm.bval = (data[i].params & (1 << j)) ? 1 : 0;
			dev.params[j] = &prm;
		}
		dev_data.devices[i] = &dev;
	}
	len = dev_data__get_packed_size(&dev_data);
	send_buf = make_buf(DEVICE_DATA_MSG, len);
	dev_data__pack(&dev_data, send_buf + 3);
	
	//send the message
	writen(nh_tcp_dawn_fd, send_buf, len + 3);
	
	//free everything
	for (int i = 0; i < num_devices; i++) {
		free(dev_data.devices[i]);
	}
	free(dev_data.devices);
	
	sleep(1); //allow time for net handler and runtime to react and generate output before returning to client
}

void print_next_dev_data ()
{
	pthread_mutex_lock(&print_udp_mutex);
	print_next_udp = 1;
	pthread_mutex_unlock(&print_udp_mutex);
	
	sleep(1); //allow time for net handler and runtime to react and generate output before returning to client
}
