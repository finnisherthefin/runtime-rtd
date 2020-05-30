/*
Constructs, encodes, and decodes the appropriate messages asked for by dev_handler.c
Previously hibikeMessage.py

Linux: json-c:      sudo apt-get install -y libjson-c-dev
       compile:     gcc -I/usr/include/json-c -L/usr/lib message.c -o message -ljson-c
       // Note that -ljson-c comes AFTER source files: https://stackoverflow.com/questions/31041272/undefined-reference-error-while-using-json-c
Mac:
       compile:     gcc -I/usr/local/include/json-c -L/usr/local/lib/ message.c -o message -ljson-c

*/
#include "message.h"

uint16_t get_device_type(dev_id_t id) {
    return id.type;
}

uint8_t get_year(dev_id_t id) {
    return id.year;
}
uint64_t get_uid(dev_id_t id) {
    return id.uid;
}

ssize_t device_data_payload_size(uint16_t device_type) {
    ssize_t result = 4; // Initialize at 1 for the 32-bit param mask
    device_t* dev = get_device(device_type);
    for (int i = 0; i < dev->num_params; i++) {
        char* type = dev->params[i].type;
        if (strcmp(type, "int") == 0) {
            result += sizeof(int);
        } else if (strcmp(type, "float") == 0) {
            result += sizeof(float);
        } else {
            result += sizeof(uint8_t);
        }
    }
    return result;
}

/***************************************************
*               MESSAGE CONSTRUCTORS               *
* Constructs message_t to be encoded and sent      *
***************************************************/
message_t* make_ping() {
    message_t* ping = malloc(sizeof(message_t));
    ping->message_id = Ping;
    ping->payload = NULL;
    ping->payload_length = 0;
    ping->max_payload_length = 0;
    return ping;
}

message_t* make_disable() {
    message_t* disable = malloc(sizeof(message_t));
    disable->message_id = Disable;
    disable->payload = NULL;
    disable->payload_length = 0;
    disable->max_payload_length = 0;
    return disable;
}

message_t* make_heartbeat_request(char heartbeat_id) {
    message_t* heartbeat_request = malloc(sizeof(message_t));
    heartbeat_request->message_id = HeartBeatRequest;
    heartbeat_request->payload = malloc(1); // Payload is 8 bits == 1 byte
    heartbeat_request->payload[0] = heartbeat_id ? heartbeat_id : 0;
    // Note that the content of payload is unused at the moment
    // May be used in the future to calculate latency
    heartbeat_request->payload_length = 1;
    heartbeat_request->max_payload_length = 1;
    return heartbeat_request;
}

message_t* make_heartbeat_response(char heartbeat_id) {
    message_t* heartbeat_response = malloc(sizeof(message_t));
    heartbeat_response->message_id = HeartBeatResponse;
    heartbeat_response->payload = malloc(1); // Payload is 8 bits == 1 byte
    heartbeat_response->payload[0] = heartbeat_id ? heartbeat_id : 0;
    // Note that the content of payload is unused at the moment
    // May be used in the future to calculate latency
    heartbeat_response->payload_length = 1;
    heartbeat_response->max_payload_length = 1;
    return heartbeat_response;
}

/*
 * Constructs a message given DEVICE_ID, array of param names PARAM_NAMES of length LEN,
 * and a DELAY in milliseconds
 * Note: Payload a 32-bit bit mask followed by the 16-bit delay
 *          --> 48-bit == 6-byte payload
*/
message_t* make_subscription_request(dev_id_t* device_id, char* param_names[], uint8_t len, uint16_t delay) {
    message_t* sub_request = malloc(sizeof(message_t));
    sub_request->message_id = SubscriptionRequest;
    sub_request->payload = malloc(6);
    sub_request->payload_length = 0;
    sub_request->max_payload_length = 6;
    // Fill in 32-bit params mask
    uint32_t mask = encode_params(device_id->type, param_names, len);
    int status = 0;
    status += append_payload(sub_request, (uint8_t*) &mask, 4);
    status += append_payload(sub_request, (uint8_t*) &delay, 2);
    return (status == 0) ? sub_request : NULL;
}

/*
 * Constructs a subscription response given DEVICE_ID, array of param names PARAM_NAMES of length LEN,
 * a DELAY in milliseconds
 * Payload: 32-bit params + 16-bit delay + 88-bit dev_id
 *          --> 136-bits == 17 bytes
*/
message_t* make_subscription_response(dev_id_t* device_id, char* param_names[], uint8_t len, uint16_t delay) {
    message_t* sub_response = malloc(sizeof(message_t));
    sub_response->message_id = SubscriptionResponse;
    sub_response->payload = malloc(17);
    sub_response->payload_length = 0;
    sub_response->max_payload_length = 17;
    uint32_t mask = encode_params(device_id->type, param_names, len);
    int status = 0;
    status += append_payload(sub_response, (uint8_t*) &mask, 4);
    status += append_payload(sub_response, (uint8_t*) &delay, 2);
    status += append_payload(sub_response, (uint8_t*) &(device_id->uid), 11);
    return (status == 0) ? sub_response : NULL;
}

/*
 * Constructs a DeviceRead message, given DEVICE_ID and array of param names PARAM_NAMES of length Len.
 * Requests the device to send data about the PARAMS
 * Payload: 32-bit param mask == 8 bytes
*/
message_t* make_device_read(dev_id_t* device_id, char* param_names[], uint8_t len) {
    message_t* dev_read = malloc(sizeof(message_t));
    dev_read->message_id = DeviceRead;
    dev_read->payload = malloc(4);
    dev_read->payload_length = 0;
    dev_read->max_payload_length = 4;
    uint32_t mask = encode_params(device_id->type, param_names, len);
    int status = 0;
    status += append_payload(dev_read, (uint8_t*) &mask, 4);
    return (status == 0) ? dev_read : NULL;
}

/*
 * Constructs a DeviceWrite message, given DEVICE_ID and an array of param_value structs to write to the device
 * Writes to the device the specified param/values pairs
 * Payload: 32-bit param mask + 32-bits for each of the 32-bit possible params = 32 + 32*32 = 1056 bits = 132 bytes
 * param_values is an array of structs
    {
    "switch0" : KEEP THE SAME
    "switch1" : 30
    "switch2" : 100
    }
*/
message_t* make_device_write(dev_id_t* device_id, param_val_t* param_values[], int len) {
    message_t* dev_write = malloc(sizeof(message_t));
    dev_write->message_id = DeviceWrite;
    dev_write->payload = malloc(device_data_payload_size(device_id->type));
    dev_write->payload_length = 0;
    dev_write->max_payload_length = 132;
    // Initialize mask to 1 if the last parameter is on. Otherwise 0
    uint32_t mask = (param_values[len-1] == NULL) ? 0 : 1;
    // Keep building the mask: 1 if present in param_values. Otherwise 0
    for (int i = len-2; i >= 0; i--) {
        mask <<= 1;
        mask |= (param_values[i] == NULL) ? 0 : 1; // Append a 1 to the right side of mask if param_values[i] is not null. Otherwise 0
    }
    int status = 0;
    // Append the mask
    status += append_payload(dev_write, (uint8_t*) &mask, 4);
    // Build the payload with the values of parameter 0 to parameter len-1
    for (int i = 0; i < len; i++) {
        if (param_values[i] == NULL) {
            continue;
        }
        device_t* dev = get_device(device_id->type);
        char* param_type = dev->params[i].type;
        if (strcmp(param_type, "int") == 0) {
            status += append_payload(dev_write, (uint8_t*) &(param_values[i]->p_i), sizeof(int));
        } else if (strcmp(param_type, "float") == 0) {
            status += append_payload(dev_write, (uint8_t*) &(param_values[i]->p_f), sizeof(float));
        } else if (strcmp(param_type, "bool") == 0) { // Boolean
            status += append_payload(dev_write, (uint8_t*) &(param_values[i]->p_b), sizeof(uint8_t));
        }
    }
    return (status == 0) ? dev_write : NULL;
}

/*
 * Returns a message with a 32-bit param mask and thirty-two 32-bit values
 * Logic is the same as make_device_write.
 */
message_t* make_device_data(dev_id_t* device_id, param_val_t* param_values[], int len) {
    message_t* dev_data = make_device_write(device_id, param_values, len);
    dev_data->message_id = DeviceData;
    return dev_data;
}

/*
 * Returns a new message with DATA as its payload
 */
message_t* make_log(char* data) {
    if ((strlen(data) + 1) > (132 * sizeof(char))) {
        printf("Error in making message: DATA IS TOO LONG.\n");
        return NULL;
    }
    message_t* log = malloc(sizeof(message_t));
    log->message_id = Log;
    log->payload = malloc(132);
    log->payload_length = 0;
    log->max_payload_length = 132;
    strcpy((char*) log->payload, data);
    log->payload_length = (uint8_t) strlen(data) + 1;
    return log;
}

message_t* make_error(uint8_t error_code) {
    message_t* error = malloc(sizeof(message_t));
    error->message_id = Error;
    error->payload = malloc(1);
    error->payload[0] = error_code;
    error->payload_length = 1;
    error->max_payload_length = 1;
    return error;
}

void destroy_message(message_t* message) {
    if (message->payload_length > 0) {
        free(message->payload);
    }
    free(message);
}

/*
 * Appends DATA with length LENGTH to the end of the payload of MSG
 * msg->payload is an array of 8-bit integers
 * msg->payload_length is the current length of the data in the payload
 * MAX_PAYLOAD_SIZE is the actual length of the payload (all of which may not be used)
 * Returns -1 if payload_length > MAX_PAYLOAD_SIZE (overwrite)
 * Returns 0 on success
 * ---This is more or less the same as Messenger::append_payload in Messenger.cpp
*/
int append_payload(message_t *msg, uint8_t *data, uint8_t length)
{
	memcpy(&(msg->payload[msg->payload_length]), data, length);
	msg->payload_length += length;
	return (msg->payload_length > msg->max_payload_length) ? -1 : 0;
}

/*
 * Compute the checksum of byte-array DATA of length LEN
*/
char checksum(char* data, int len) {
    char chk = data[0];
    for (int i = 0; i < len; i++) {
        chk ^= data[i];
    }
    return chk;
}

/***************************************************
*               COBS ENCODE/DECODE
* Implementation copied from Messenger.cpp in lowcar
***************************************************/
/*
 * A macro to help with cobs_encode
*/
#define finish_block() {		\
	*block_len_loc = block_len; \
	block_len_loc = dst++;      \
	out_len++;                  \
	block_len = 0x01;           \
}

/*
Encodes src into dst and returns the size of dst. src nust not overlap dst.
Replace all zero bytes with nonzero bytes describing the location of the next zero
For detailed example, view https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing#Encoding_examples
*/
ssize_t cobs_encode(uint8_t *dst, const uint8_t *src, ssize_t src_len)
{
	const uint8_t *end = src + src_len;
	uint8_t *block_len_loc = dst++;
	uint8_t block_len = 0x01;
	ssize_t out_len = 0;

	while (src < end) {
		if (*src == 0) {
			finish_block();
		} else {
			*dst++ = *src;
			block_len++;
			out_len++;
			if (block_len == 0xFF) {
				finish_block();
			}
		}
		src++;
	}
	finish_block();

	return out_len;
}

/*
 * Decodes src into dst and returns the size of dst. src may overlap dst.
 Revert back into original non-cobs_encoded data
 For example, view https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing#Encoding_examples
*/
ssize_t cobs_decode(uint8_t *dst, const uint8_t *src, ssize_t src_len)
{
	const uint8_t *end = src + src_len;
	ssize_t out_len = 0;

	while (src < end) {
		uint8_t code = *src++;
		for (uint8_t i = 1; i < code; i++) {
			*dst++ = *src++;
			out_len++;
			if (src > end) { // Bad packet
				return 0;
			}
		}
		if (code < 0xFF && src != end) {
			*dst++ = 0;
			out_len++;
		}
	}
	return out_len;
}

/*
 * Given string array PARAMS of length LEN consisting of params of DEVICE_UID,
 * Generate a bit-mask of 1s (if param is in PARAMS), 0 otherwise
 * Ex: encode_params(0, ["switch2", "switch1"], 2)
 *  Switch2 is param 2, switch1 is param 1
 *  Return 110  (switch2 on, switch1 on, switch0 off)
*/
uint32_t encode_params(uint16_t device_type, char** params, uint8_t len) {
    uint8_t param_nums[len]; // [1, 9, 2] -> [0001, 1001, 0010]
    device_t* dev = get_device(device_type);
    int num_params_in_dev = dev->num_params;
    // Iterate through PARAMS and find its corresponding number in the official list
    for (int i = 0; i < len; i++) {
        // Iterate through official list of params to find the param number
        for (int j = 0; j < num_params_in_dev; j++) {
            if (strcmp(dev->params[j].name, params[i]) == 0) { // Returns 0 if they're equivalent
                param_nums[i] = j; // DEVICES[device_type]->params[j].number;
                break;
            }
        }
    }
    // Generate mask by OR-ing numbers in param_nums
    uint32_t mask = 0;
    for (int i = 0; i < len; i++) {
        mask = mask | (1 << param_nums[i]);
    }
    return mask;
}

/*
 * Given a device_type and a mask, return an array of param names
 * Encode 1, 9, 2 --> 1^9^2 = 10
 * Decode 10 --> 1, 9, 2
*/
/* To be determined; not yet sure how we want to return an array of strings
char** decode_params(uint16_t device_type, uint32_t mask) {
    // Iterate through MASK to find the name of the parameter
    int len = 0;
    int copy = mask; // 111 --> ["switch0", "switch1", "switch2"]
    // Count the number of bits that are on (number of params encoded)
    while (copy != 0) {
        len += copy & 1;
        copy >>= 1;
    }
    char* param_names[len];
    int i = 0;
    // Get the names of the params
    for (int j = 0; j < 32; j++) {
        if (mask & (1 << j)) { // If jth bit is on (from the right)
            param_names[i] = DEVICES[device_type]->params[j].name;
            i++;
        }
    }
    return param_names;
}
*/

void message_to_bytes(message_t* msg, char data[], int len) {
    // Initialize the byte array. See page 8 of book.pdf
    // 1 byte for messageID, 1 byte for payload length, the payload itself, and 1 byte for the checksum
    if (len < 3 + msg->payload_length) {
        printf("Error converting to bytes. LEN is too small\n");
        return;
    }
    data[0] = msg->message_id;
    data[1] = msg->payload_length;
    for (int i = 0; i < msg->payload_length; i++) {
        data[i+2] = msg->payload[i];
    }
    data[2 + msg->payload_length] = checksum(data, 2 + msg->payload_length);
}

int parse_message(char data[], message_t* msg_to_fill) {
    msg_to_fill->message_id = data[0];
    msg_to_fill->payload_length = data[1];
    msg_to_fill->max_payload_length = data[1];
    for (int i = 0; i < msg_to_fill->payload_length; i++) {
        msg_to_fill->payload[i] = data[i + 2];
    }
    char expected_checksum = data[2 + msg_to_fill->payload_length];
    char received_checksum = checksum(data, 2 + msg_to_fill->payload_length);
    return (expected_checksum != received_checksum) ? 1 : 0;
}
