#include "dev_handler.h"

// ************************************ PRIVATE TYPEDEFS ****************************************** //

/*
 * A struct defining a device's information for the lifetime of its connection
 * Fields can be easily retrieved without requiring libusb_open()
 */
typedef struct usb_id {
    uint16_t vendor_id;    // Obtained from struct libusb_device_descriptor field from libusb_get_device_descriptor()
    uint16_t product_id;   // Same as above
    uint8_t dev_addr;      // Obtained from libusb_get_device_address(libusb_device*)
} usb_id_t;

/* Struct shared between the three threads so they can communicate with each other
 * relayer thread acts as the "control center" and connects/disconnects to shared memory
 *  relayer thread also frees memory when device is disconnected
 */
typedef struct msg_relay {
    libusb_device* dev;
    libusb_device_handle* handle;
    int interface_idx;      // The index of the USB interface claimed on the device
    pthread_t sender;
    pthread_t receiver;
    pthread_t relayer;
    uint8_t send_endpoint;
    uint8_t receive_endpoint;
    int shm_dev_idx;        // The unique index assigned to the device by shm_wrapper for shared memory operations on device_connect()
    uint8_t start;		    // set by relayer: A flag to tell reader and receiver to start work when set to 1
    dev_id_t dev_id;        // set by relayer once SubscriptionResponse is received
    uint64_t sent_hb_req;	// set by sender: The TIME that a HeartBeatRequest was sent. Relay should expect a hb response
    uint8_t got_hb_req;	// set by receiver: A flag to tell sender to send a HeartBeatResponse
} msg_relay_t;

// ************************************ PRIVATE FUNCTIONS ****************************************** //
// Polling Utility
usb_id_t** alloc_tracked_devices(libusb_device** lst, int len);
void free_tracked_devices(usb_id_t** lst, int len);
libusb_device* get_new_device(libusb_device** connected, int num_connected, usb_id_t** tracked, int num_tracked);

// Thread-handling for communicating with devices
void communicate(libusb_device* dev);
void* relayer(void* relay_cast);
void relay_clean_up(msg_relay_t* relay);
void* sender(void* relay_cast);
void* receiver(void* relay_cast);

// Device communication (Bulk transferring)
int get_endpoints(msg_relay_t* relay, struct libusb_interface interface);
int send_message(message_t* msg, libusb_device_handle* handle, uint8_t endpoint, int* transferred);
int receive_message(message_t* msg, libusb_device_handle* handle, uint8_t endpoint);
int ping(msg_relay_t* relay);

// Utility
int64_t millis();
void print_bytes(uint8_t* data, int len);

// ************************************ PUBLIC FUNCTIONS ****************************************** //

// Initialize data structures / connections
void init() {
	// Init shared memory
	// shm_init(DEV_HANDLER);
	// Init libusb
	int ret;
	if ((ret = libusb_init(NULL)) != 0) {
		printf("libusb_init failed on exit code %d\n", ret);
		libusb_exit(NULL);
		return;
	}
}

// Free memory and safely stop connections
void stop() {
	printf("\nINFO: Ctrl+C pressed. Safely terminating program\n");
    fflush(stdout);
	// Exit libusb
	libusb_exit(NULL);
	// TODO: For each tracked lowcar device, disconnect from shared memory
	// TODO: Stop shared memory
	// shm_stop(DEV_HANDLER);
	exit(0);
}

/*
 * Detects when devices are connected and disconnected.
 * On lowcar device connect, connect to shared memory and spawn three threads to communicate with the device
 * On lowcar device disconnect, disconnects the device from shared memory.
 */
void poll_connected_devices() {
	// Initialize variables
	libusb_device* dev = NULL;
	libusb_device** connected;	 										// List of connected device in this iteration
	printf("INFO: Getting initial device list\n");
	int num_tracked = libusb_get_device_list(NULL, &connected);			// Number of connected devices in previous iteration
	usb_id_t** tracked = alloc_tracked_devices(connected, num_tracked);	// List of connected deevices in previous iteration
	libusb_free_device_list(connected, 1);
	int num_connected;													// Number of connected devices in this iteration

	// Initialize timer
	// time_t current_time;

	// Poll
	printf("INFO: Variables initialized. Polling now.\n");
	while (1) {
		// Check how many are connected
		num_connected = libusb_get_device_list(NULL, &connected);
		if (num_connected == num_tracked) {
			libusb_free_device_list(connected, 1);
			continue;
		} else {
			if (num_connected > num_tracked) {
				// Communicate with the new device
				printf("INFO: NEW DEVICE CONNECTED\n");
				dev = get_new_device(connected, num_connected, tracked, num_tracked);
				communicate(dev);
			} else if (num_connected < num_tracked) {
				printf("INFO: DEVICE  DISCONNECTED\n");
			}
			// Update tracked devices
			free_tracked_devices(tracked, num_tracked);
			tracked = alloc_tracked_devices(connected, num_connected);
			num_tracked = num_connected;
			// Free connected list
			libusb_free_device_list(connected, 1);
		}
	}
}

// ************************************ POLLING UTILITY ****************************************** //

/*
 * Allocates memory for an array of usb_id_t and populates it with data from the provided list
 * lst: Array of libusb_devices to have their vendor_id, product_id,
 *      and device address to be put in the result
 * len: The length of LST
 * return: Array of usb_id_t pointers of length LEN. Must be freed with free_tracked_devices()
 */
usb_id_t** alloc_tracked_devices(libusb_device** lst, int len) {
 	struct libusb_device_descriptor desc;
 	usb_id_t** result = malloc(len * sizeof(usb_id_t*));
 	printf("INFO: There are now %d tracked devices.\n", len);
 	for (int i = 0; i < len; i++) {
 		result[i] = malloc(sizeof(usb_id_t));
 		libusb_device* dev = lst[i];
 		libusb_get_device_descriptor(dev, &desc);
 		result[i]->vendor_id = desc.idVendor;
 		result[i]->product_id = desc.idProduct;
 		result[i]->dev_addr = libusb_get_device_address(dev);
 	}
 	return result;
}

/*
 * Frees the given list of usb_id_t pointers
 */
void free_tracked_devices(usb_id_t** lst, int len) {
	for (int i = 0; i < len; i++) {
		free(lst[i]);
	}
	free(lst);
}

/*
 * Returns a pointer to the libusb_device in CONNECTED but not in TRACKED
 * connected: List of libusb_device*
 * num_connected: Length of CONNECTED
 * tracked: List of usb_id_t* that holds info about every device in CONNECTED except for one
 * num_tracked: Length of TRACKED. Expected to be exactly one less than connected
 * return: Pointer to libusb_device
 */
libusb_device* get_new_device(libusb_device** connected, int num_connected, usb_id_t** tracked, int num_tracked) {
 	libusb_device* dev;
 	struct libusb_device_descriptor desc;
 	// Loop through each connected device
 	for (int i = 0; i < num_connected; i++) {
 		dev = connected[i];
 		libusb_get_device_descriptor(dev, &desc);
 		uint16_t vendor_id = desc.idVendor;
 		uint16_t product_id = desc.idProduct;
 		uint8_t dev_addr = libusb_get_device_address(dev);
 		// If it's not in tracked, it's a new device
 		uint8_t in_tracked = 0;
 		for (int j = 0; j < num_tracked; j++) {
 			if (vendor_id == tracked[i]->vendor_id &&
 				product_id == tracked[i]->product_id &&
 				dev_addr == tracked[i]->dev_addr) {
 					in_tracked = 1;
 					break;
 				}
 		}
 		if (in_tracked == 0) {
 			libusb_get_device_descriptor(dev, &desc);
 			printf("INFO:    Vendor:Product: %u:%u\n", desc.idVendor, desc.idProduct);
 			return connected[i];
 		}
 	}
 	return NULL;
}

// ************************************ THREAD-HANDLING ****************************************** //

/*
 * Opens threads for communication with a device
 * Three threads will be opened:
 *  relayer: Verifies device is lowcar and cancels threads when device disconnects/timesout
 *  sender: Sends changed data in shared memory and periodically sends HeartBeatRequests
 *  receiver: Receives data from the lowcar device and signals sender thread about HeartBeatRequests
 *
 * dev: The libusb_device to communicate with
 */
void communicate(libusb_device* dev) {
	msg_relay_t* relay = malloc(sizeof(msg_relay_t));
	relay->dev = dev;
	// Open the device
	int ret = libusb_open(dev, &relay->handle);
	if (ret != 0) {
		printf("ERROR: libusb_open in communicate() with error code %s\n", libusb_error_name(ret));
	}
	// Initialize relay->start as 0, indicating sender and receiver to not start work yet
	relay->start = 0;
	// Initialize the other relay values
	relay->interface_idx = -1;
	relay->send_endpoint = -1;
	relay->receive_endpoint = -1;
	relay->shm_dev_idx = -1;
	relay->dev_id.type = -1;
	relay->dev_id.year = -1;
	relay->dev_id.uid = -1;
	relay->sent_hb_req = 0;
	relay->got_hb_req= 0;
	// Open threads for sender, receiver, and relayer
	pthread_create(&relay->sender, NULL, sender, relay);
	pthread_create(&relay->receiver, NULL, receiver, relay);
	pthread_create(&relay->relayer, NULL, relayer, relay);
}

/*
 * Sends a Ping to the device and waits for a SubscriptionResponse
 * If the SubscriptionResponse takes too long, close the device and exit all threads
 * Connects the device to shared memory and signals the sender and receiver to start
 * Continuously checks if the device disconnected or HeartBeatRequest was sent without a response
 *      If so, it disconnects the device from shared memory, closes the device, and frees memory
 * RELAY_CAST is to be casted as msg_relay_t
 */
void* relayer(void* relay_cast) {
	msg_relay_t* relay = relay_cast;
	int ret;
	// Get the device's active config to find the number of interfaces it has
	struct libusb_config_descriptor* config;
	ret = libusb_get_active_config_descriptor(relay->dev, &config);
	if (ret != 0) {
		printf("ERROR: Couldn't get active config descriptor with error code %s\n", libusb_error_name(ret));
		relay_clean_up(relay);
		return NULL;
	}
	printf("INFO: Device has %d interfaces\n", config->bNumInterfaces);

	// Tell libusb to automatically detach the kernel driver on claimed interfaces then reattach them when releasing the interface
	// This function is not supported for Darwin or Windows, and will simply do nothing
	libusb_set_auto_detach_kernel_driver(relay->handle, 1);

	/* Claim an interface on the device's handle, telling OS we want to do I/O on the device
	 * If we couldn't get the send endpoint and receive endpoint, try another interface to claim. */
	for (int i = 0; i < config->bNumInterfaces; i++) {
		ret = libusb_claim_interface(relay->handle, i);
		if (ret != 0) {
			printf("INFO: Couldn't claim interface %d with error code %s\n", i, libusb_error_name(ret));
			continue;
		}
		printf("INFO: Successfully claimed interface %d!\n", i);

		// If we're able to get the endpoints, set relay->interface_idx and proceed to send a Ping
		if (get_endpoints(relay, config->interface[i]) == 0) {
			relay->interface_idx = i;
			break;
		}
		// If we couldn't get the endpoints, release this interface and try another
		libusb_release_interface(relay->handle, i);
		// If we tried all interfaces and couldn't get the endpoints, we give up :(
		if (i == config->bNumInterfaces - 1) {
			printf("FATAL: Couldn't identify endpoints for this device :( Giving up.\n");
			relay_clean_up(relay);
			return NULL;
		}
	}

	// Send a Ping and wait for a SubscriptionResponse
	printf("INFO: Relayer will send a Ping to the device\n");
	ret = ping(relay);
	if (ret != 0) {
		relay_clean_up(relay);
		return NULL;
	}

	/****** At this point, the Ping/SubscriptionResponse handshake was successful! ******/

	// TODO: Connect the lowcar device to shared memory
	printf("INFO: Relayer will connect the device to shared memory\n");
	// device_connect(relay->dev_id.type, relay->dev_id.year, relay->dev_id.uid, &relay->shm_dev_idx);

	// Signal the sender and receiver to start work
	printf("INFO: Relayer broadcasting to Sender and Receiver to start work\n");
	relay->start = 1;

	// If the device disconnects or a HeartBeatRequest takes too long to be resolved, clean up
	printf("INFO: Relayer will begin to monitor the device\n");
	struct libusb_device_descriptor desc;
	while (1) {
		// Getting the device descriptor if the device is disconnected should give an error
		ret = libusb_get_device_descriptor(relay->dev, &desc);
		if ((ret != 0) || ((millis() - relay->sent_hb_req) >= DEVICE_TIMEOUT)) {
			printf("INFO: Device disconnected or timed out!\n");
			relay_clean_up(relay);
			return NULL;
		}
	}
}

/* Called by relayer to clean up after the device.
 * Releases interface, closes device, cancels threads,
 * disconnects from shared memory, and frees the RELAY struct
 */
void relay_clean_up(msg_relay_t* relay) {
	printf("INFO: Cleaning up threads\n");
	// Release the device interface. If it was never claimed, this will do nothing.
	libusb_release_interface(relay->handle, relay->interface_idx);
	// Close the device
	libusb_close(relay->handle);
	// Cancel the sender and receiver threads when ongoing transfers are completed
	pthread_cancel(relay->sender);
	pthread_cancel(relay->receiver);
	// Sleep to ensure that the sender and receiver threads are cancelled before disconnecting from shared memory / freeing relay
	sleep(1);
	// Disconnect the device from shared memory if it's connected
	if (relay->shm_dev_idx != -1) {
		// device_disconnect(relay->shm_dev_idx)
	}
	free(relay);
	printf("INFO: Cleaned up threads\n");
	pthread_cancel(pthread_self());
}

/*
 * Continuously reads from shared memory to serialize the necessary information
 * to send to the device.
 * Sets relay->sent_hb_req when a HeartBeatRequest is sent
 * Sends a HeartBeatResponse when relay->got_hb_req is set
 * RELAY_CAST is to be casted as msg_relay_t
 */
void* sender(void* relay_cast) {
	msg_relay_t* relay = relay_cast;
	// Sleep until relay->start == 1, which is true when relayer receives a SubscriptionResponse
	while (1) {
		if (relay->start == 1) {
			break;
		}
		pthread_testcancel(); // Cancellation point. Relayer may cancel this thread if device timesout
	}

	/* Do work! Continuously read shared memory param bitmap. If it changes,
	 * Call device_read to get the changed parameters and send them to the device
	 * Also, send HeartBeatRequest at a regular interval, HB_REQ_FREQ.
	 * Finally, send HeartBeatResponse when receiver gets a HeartBeatRequest */
	// pmap: Array of thirty-three uint32_t.
	//       Indices 1-32 are bitmaps indicating which parameters have new values to be written to the device at that index
	//       pmap[0] is a bitmap indicating which devices have new values to be written
	uint32_t* pmap;
	param_val_t* params = malloc(MAX_PARAMS * sizeof(param_val_t)); // Array of params to be filled on device_read
	message_t* msg; 		// Message to build
	int transferred = 0; 	// The number of bytes actually transferred
	int ret; 				// Hold the value from send_message()
	while (1) {
		// Get the current param bitmap
		// get_param_bitmap(pmap);
		/* If pmap[0] != 0, there are devices that need to be written to.
		 * This thread is interested in only whether or not THIS device requires being written to.
		 * If the device's shm_dev_idx bit is on in pmap[0], there are values to be written to the device. */
		if (pmap[0] & (1 << relay->shm_dev_idx)) {
			// Read the new parameter values from shared memory as DEV_HANDLER from the COMMAND stream
			// device_read(relay->shm_dev_idx, DEV_HANDLER, COMMAND, pmap[1 + relay->shm_dev_idx], params);
			// Serialize and bulk transfer a DeviceWrite packet with PARAMS to the device
			msg = make_device_write(&relay->dev_id, pmap[1 + relay->shm_dev_idx], params);
			ret = send_message(msg, relay->handle, relay->send_endpoint, &transferred);
			if (ret != 0) {
				printf("ERROR: DEVICE_WRITE transfer failed with error code %s\n", libusb_error_name(ret));
			}
			destroy_message(msg);
		}
		pthread_testcancel(); // Cancellation point
		// If it's been HB_REQ_FREQ milliseconds since last sending a HeartBeatRequest, send another one
		if ((millis() - relay->sent_hb_req) >= HB_REQ_FREQ) {
			msg = make_heartbeat_request(0);
			send_message(msg, relay->handle, relay->send_endpoint, &transferred);
			if (ret != 0) {
				printf("ERROR: HeartBeatRequest transfer failed with error code %s\n", libusb_error_name(ret));
			}
			destroy_message(msg);
		}
		pthread_testcancel(); // Cancellation point
		// If receiver got a HeartBeatRequest, send a HeartBeatResponse and mark the request as resolved
		if (relay->got_hb_req) {
			msg = make_heartbeat_response(0);
			send_message(msg, relay->handle, relay->send_endpoint, &transferred);
			if (ret != 0) {
				printf("ERROR: HeartBeatResponse transfer failed with error code %s\n", libusb_error_name(ret));
			}
			destroy_message(msg);
			relay->got_hb_req = 0; // Mark as resolved
		}
		pthread_testcancel(); // Cancellation point
	}
	return NULL;
}

/*
 * Continuously attempts to parse incoming data over serial and send to shared memory
 * Sets relay->got_hb_req upon receiving a HeartBeatRequest
 * RELAY_CAST is to be casted as msg_relay_t
 */
void* receiver(void* relay_cast) {
	msg_relay_t* relay = relay_cast;
	// Sleep until relay->start == 1, which is true when relayer receives a SubscriptionResponse
	while (1) {
		if (relay->start == 1) {
			break;
		}
		pthread_testcancel(); // Cancellation point. Relayer may cancel this thread if device timesout
	}
	/* Do work! Continuously read from the device until a complete message can be parsed.
	 * If it's a HeartBeatRequest, signal sender to reply with a HeartBeatResponse
	 * If it's a HeartBeatResponse, mark the current HeartBeatRequest as resolved
	 * If it's device data, write it to shared memory
	 */
	// An empty message to parse the received data into
	message_t* msg = make_empty(MAX_PAYLOAD_SIZE);
	// An array of empty parameter values to be populated from DeviceData message payloads and written to shared memory
	param_val_t* vals = malloc(MAX_PARAMS * sizeof(param_val_t));
	while (1) {
		pthread_testcancel(); // Cancellation point
		// Try to read a message
		if (receive_message(msg, relay->handle, relay->receive_endpoint) != 0) {
			continue;
		}
		if (msg->message_id == HeartBeatRequest) {
			// If it's a HeartBeatRequest, take note of it so sender can resolve it
			relay->got_hb_req = 1;
		} else if (msg->message_id == HeartBeatResponse) {
			// If it's a HeartBeatResponse, mark the current HeartBeatRequest as resolved
			relay->sent_hb_req = 0;
		} else if (msg->message_id == DeviceData) {
			// First, parse the payload into an array of parameter values
			parse_device_data(relay->dev_id.type, msg, vals); // This will overwrite the values in VALS that are indicated in the bitmap
			// TODO: Now write it to shared memory
			// device_write(relay->shm_dev_idx, DEV_HANDLER, DATA, bitmap, vals);
		} else if (msg->message_id == Log) {
			// TODO: Send payload to logger
		} else if (msg->message_id == Error) {
			// TODO: Send payload to logger
		} else {
			// Received a message of unexpected type.
			printf("FATAL: Received a message of unexpected type and dropping it.\n");
		}
		// Now that the message is taken care of, clear the message
		msg->message_id = 0x0;
		msg->payload = 0x0;
		msg->payload_length = 0;
	    msg->max_payload_length = MAX_PAYLOAD_SIZE;
	}
	return NULL;
}

// ************************************ DEVICE COMMUNICATION ****************************************** //

/*
 * Populates RELAY's send_endpoint and receive_endpoint if possible
 * relay: msg_relay_t struct to be filled with the endpoints
 * interface: A libusb interface containing (possibly invalid for bulk transfer) endpoints
 * return:	0 if successful
 *			1 otherwise
 */
int get_endpoints(msg_relay_t* relay, struct libusb_interface interface) {
	// Get the 0th setting of the claimed interface. TODO: Getting the first one. Change later?
	printf("INFO: This interface has %d settings\n", interface.num_altsetting);
	const struct libusb_interface_descriptor interface_desc = interface.altsetting[0];

	// Make sure we have at least 2 endpoints on this interface setting
	printf("INFO: 0th setting has %d endpoints\n", interface_desc.bNumEndpoints);
	if (interface_desc.bNumEndpoints < 2) {
		printf("INFO: This interface setting has fewer than 2 endpoints! Giving up on this interface\n");
		return 1;
	}

	// Initialize flags to indicate whether we are able to find the endpoints
	uint8_t found_bulk_in = 0;
	uint8_t found_bulk_out = 0;
	struct libusb_endpoint_descriptor endpoint;

	// Iterate through the endpoints until we get both bulk endpoint in and bulk endpoint out
	for (int i = 0; i < interface_desc.bNumEndpoints; i++) {
		endpoint = interface_desc.endpoint[i];
		// Bits 0 and 1 of endpoint.bmAttributes detail the transfer type. We want bulk
		if ((3 & endpoint.bmAttributes) != LIBUSB_TRANSFER_TYPE_BULK) {
			continue;
		}
		// Bit 7 of endpoint.bEndpointAddress indicates the direction
		if (!found_bulk_in && (((1 << 7) & endpoint.bEndpointAddress) == LIBUSB_ENDPOINT_IN)) {
			// LIBUSB_ENDPOINT_IN: Transfer from device to dev_handler
			relay->receive_endpoint = endpoint.bEndpointAddress;
			found_bulk_in = 1;
		} else if (!found_bulk_out && (((1 << 7) & endpoint.bEndpointAddress) == LIBUSB_ENDPOINT_OUT)) {
			// LIBUSB_ENDPOINT_OUT: Transfer from dev_handler to device
			relay->send_endpoint = endpoint.bEndpointAddress;
			found_bulk_out = 1;
		}
		// If we found both endpoints, return
		if (found_bulk_in && found_bulk_out) {
			printf("INFO: Endpoints were successfully identified!\n");
			printf("INFO:     Send (OUT) Endpoint: 0x%X\n", relay->send_endpoint);
			printf("INFO:     Receive (IN) Endpoint: 0x%X\n", relay->receive_endpoint);
			return 0;
		}
	}
	if (!found_bulk_in) {
		printf("FATAL: Couldn't get endpoint for receiver\n");
	}
	if (!found_bulk_out) {
		printf("FATAL: Couldn't get endpoint for sender\n");
	}
	return 1;
}

/*
 * Serializes and sends a message to the specified endpoint
 * msg: The message to be sent, constructed from the functions below
 * handle: A libusb device handle obtained by using libusb_open on the device
 * endpoint: The usb endpoint address that the message should be sent to
 * transferred: Output location for number of bytes actually sent
 * return: The return value of libusb_bulk_transfer
 *      http://libusb.sourceforge.net/api-1.0/group__libusb__syncio.html#gab8ae853ab492c22d707241dc26c8a805
 */
int send_message(message_t* msg, libusb_device_handle* handle, uint8_t endpoint, int* transferred) {
	int len = calc_max_cobs_msg_length(msg);
	uint8_t* data = malloc(len);
	len = message_to_bytes(msg, data, len);
	int ret = libusb_bulk_transfer(handle, endpoint, data, len, transferred, DEVICE_TIMEOUT);
	free(data);
	return ret;
}

/*
 * Attempts to read a message from the specified endpoint
 * msg: Output empty message to be populated with the read message if successful
 * handle: A libusb device handle obtained by using libusb_open on the device
 * endpoint: The usb endpoint address that the message should be read from
 * return: 0 if a full message was successfully read and put into MSG
 *         1 if no message was found
 *         2 if a broken message was received
 *         3 if a message was received but with an incorrect checksum
 */
int receive_message(message_t* msg, libusb_device_handle* handle, uint8_t endpoint) {
	// Allocate a buffer with enough space for the largest message + a byte of cobs encoding overhead
	uint8_t* data = malloc(MESSAGE_ID_SIZE + PAYLOAD_LENGTH_SIZE + MAX_PAYLOAD_SIZE + CHECKSUM_SIZE + 1);
	// Variable to temporarily hold a read byte
	uint8_t last_byte_read;
	// Holds the actual number of bytes read from a transfer
	int transferred;
	// Try to read a byte from the device
	int ret = libusb_bulk_transfer(handle, endpoint, &last_byte_read, 1, &transferred, DEVICE_TIMEOUT);
	// If the byte read is the delimiter, (0x0) then we have an incoming message!
	if (ret == LIBUSB_SUCCESS && transferred == 1) {
		printf("INFO: A byte was received from the device!\n");
		if (last_byte_read == 0) {
			printf("INFO: It's the start of a message!\n");
			// Read the length of the message
			ret = libusb_bulk_transfer(handle, endpoint, &last_byte_read, 1, &transferred, DEVICE_TIMEOUT);
			if (ret == LIBUSB_SUCCESS && transferred == 1) {
				int cobs_len = last_byte_read;
				printf("INFO: Received the length of the encoded message: %d bytes.\n", cobs_len);
				// Read the encoded message
				ret = libusb_bulk_transfer(handle, endpoint, data, cobs_len, &transferred, DEVICE_TIMEOUT);
				if (ret == LIBUSB_SUCCESS && transferred == cobs_len) {
					// Parse the read message
					printf("INFO: Received the full message and will now parse\n");
					if (parse_message(data, msg) == 0) {
						printf("INFO: Message of type %X successfully parsed!\n", msg->message_id);
						return 0;
					} else {
						printf("FATAL: The data received has an incorrect checksum. Dropping it.\n");
						return 3;
					}
				} else {
					printf("FATAL: Didn't receive the full message. Dropping it.\n");
					return 2;
				}
			} else {
				printf("FATAL: Didn't receive the length of the message. Dropping it.\n");
				return 2;
			}
		} else {
			printf("FATAL: Received a stray byte. Dropping it.\n");
			return 2;
		}
	}
	return 1;
}

/*
 * Synchronously sends a PING message to a device to check if it's a lowcar device
 * relay: Struct holding device, handle, and endpoint fields
 * return: 0 upon successfully receiving a SubscriptionResponse and setting relay->dev_id
 *      1 if Ping message couldn't be serialized
 *      2 if Ping message couldn't be sent or SubscriptionResponse wasn't received
 *      3 if received message has incorrect checksum
 *      4 if received message is not a SubscriptionResponse
 */
int ping(msg_relay_t* relay) {
	// Make a PING message to be sent over serial
	message_t* ping = make_ping();
	int len = calc_max_cobs_msg_length(ping);
	uint8_t* data = malloc(len * sizeof(uint8_t));
	int ret = message_to_bytes(ping, data, len);
	destroy_message(ping);
	// ret holds the length of data (the length of the serialized Ping message)
	if (ret <= 0) {
		printf("ERROR: Couldn't serialize ping message in ping()\n");
		free(data);
		return 1;
	}

	// Bulk transfer the Ping message to the device
	printf("INFO: Ping message serialized and ready to transfer\n");
	print_bytes(data, ret);
	int transferred = 0; // The number of bytes actually sent
	ret = libusb_bulk_transfer(relay->handle, relay->send_endpoint, data, ret, &transferred, DEVICE_TIMEOUT);
	if (ret != 0) {
		printf("ERROR: Couldn't bulk transfer Ping with error code %s\n", libusb_error_name(ret));
		free(data);
		return 2;
	}
	free(data);
	data = NULL;
	printf("INFO: Ping message successfully sent with %d bytes!\n", transferred);

	// Try to read a SubscriptionResponse, which we expect from a lowcar device that receives a Ping
	data = malloc(32); // It shouldn't be more than 32 bytes. ~20 + cobs
	printf("INFO: Listening for SubscriptionResponse\n");
	ret = libusb_bulk_transfer(relay->handle, relay->receive_endpoint, data, 32, &transferred, DEVICE_TIMEOUT);
	if (ret != 0) {
		printf("ERROR: Couldn't bulk transfer SubscriptionResponse with error code %s\n", libusb_error_name(ret));
		free(data);
		return 2;
	}
	// Parse the received message to verify it's a SubscriptionResponse
	printf("INFO: %d bytes of data received and will be parsed!\n", transferred);
	print_bytes(data, transferred);
	message_t* sub_response = malloc(sizeof(message_t));
	ret = parse_message(data, sub_response);
	free(data); // We don't need the encoded message anymore
	if (ret != 0) {
		printf("FATAL: Received data with incorrect checksum\n");
		free(sub_response);
		return 3;
	} else if (sub_response->message_id != SubscriptionResponse) {
		printf("FATAL: Message is not a SubscriptionResponse\n");
		free(sub_response);
		return 4;
	}
	// We have a SubscriptionResponse!
	printf("INFO: SubscriptionResponse received!\n");
	relay->dev_id.type = ((uint16_t*)(&sub_response->payload[6]))[0];	// device type is the 16-bits starting at the 6th byte
	relay->dev_id.year = sub_response->payload[8];						// device year is the 8th byte
	relay->dev_id.uid  = ((uint64_t*)(&sub_response->payload[9]))[0];	// device uid is the 64-bits starting at the 9th byte
	return 0;
}

// ************************************ UTILITY ****************************************** //

/* Returns the number of milliseconds since the Unix Epoch */
int64_t millis() {
	struct timeval time; // Holds the current time in seconds + microsecondsx
	gettimeofday(&time, NULL);
	int64_t s1 = (int64_t)(time.tv_sec) * 1000; // Convert seconds to milliseconds
	int64_t s2 = (time.tv_usec / 1000);			// Convert microseconds to milliseconds
	return s1 + s2;
}

/* Prints the byte array DATA of length LEN in hex */
void print_bytes(uint8_t* data, int len) {
	printf("INFO: Data: 0x");
	for (int i = 0; i < len; i++) {
		printf("%X", data[i]);
	}
	printf("\n");
}

// ************************************ MAIN ****************************************** //

int main() {
	// If SIGINT (Ctrl+C) is received, call sigintHandler to clean up
	signal(SIGINT, stop);
	init();
	// Set libusb to log debug messages
	// if (libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING) != LIBUSB_SUCCESS) {
	// 	printf("ERROR: libusb log level could not be set\n");
	// }
	// Start polling
	poll_connected_devices();
	return 0;
}
