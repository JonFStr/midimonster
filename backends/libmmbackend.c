#include "libmmbackend.h"

#define LOGPF(format, ...) fprintf(stderr, "libmmbe\t" format "\n", __VA_ARGS__)
#define LOG(message) fprintf(stderr, "libmmbe\t%s\n", (message))

#ifndef _WIN32
	#define closesocket close
#endif

int mmbackend_strdup(char** dest, char* src){
	if(*dest){
		free(*dest);
	}

	*dest = strdup(src);

	if(!*dest){
		LOG("Failed to allocate memory");
		return 1;
	}
	return 0;
}

char* mmbackend_socket_strerror(int err_no){
	#ifdef _WIN32
	static char error[2048] = "";
	ssize_t u; 
	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, WSAGetLastError(),
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), error, sizeof(error), NULL);
	//remove trailing newline that for some reason is included in most of these...
	for(u = strlen(error) - 1; u > 0; u--){
		if(!isprint(error[u])){
			error[u] = 0;
		}
	}
	return error;
	#else
	return strerror(err_no);
	#endif
}

const char* mmbackend_sockaddr_ntop(struct sockaddr* peer, char* buffer, size_t length){
	union {
		struct sockaddr* in;
		struct sockaddr_in* in4;
		struct sockaddr_in6* in6;
	} addr;
	addr.in = peer;
	#ifdef _WIN32
	uint8_t* data = NULL;
	#endif

	switch(addr.in->sa_family){
		//inet_ntop has become available in the winapi with vista, but eh.
		#ifdef _WIN32
		case AF_INET6:
			data = addr.in6->sin6_addr.s6_addr;
			snprintf(buffer, length, "%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X:%02X%02X",
					data[0], data[1], data[2], data[3],
					data[4], data[5], data[6], data[7],
					data[8], data[9], data[10], data[11],
					data[12], data[13], data[14], data[15]);
			return buffer;
		case AF_INET:
			data = (uint8_t*) &(addr.in4->sin_addr.s_addr);
			snprintf(buffer, length, "%d.%d.%d.%d", data[0], data[1], data[2], data[3]);
			return buffer;
		#else
		case AF_INET6:
			return inet_ntop(addr.in->sa_family, &(addr.in6->sin6_addr), buffer, length);
		case AF_INET:
			return inet_ntop(addr.in->sa_family, &(addr.in4->sin_addr), buffer, length);
		#endif
		default:
			snprintf(buffer, length, "Socket family not implemented");
			return buffer;
	}
}

void mmbackend_parse_hostspec(char* spec, char** host, char** port, char** options){
	size_t u = 0;

	if(!spec || !host || !port){
		return;
	}

	*port = NULL;

	//skip leading spaces
	for(; spec[u] && isspace(spec[u]); u++){
	}

	if(!spec[u]){
		*host = NULL;
		return;
	}

	*host = spec + u;

	//scan until string end or space
	for(; spec[u] && !isspace(spec[u]); u++){
	}

	//if space, the rest should be the port
	if(spec[u]){
		spec[u] = 0;
		*port = spec + u + 1;
	}

	if(options){
		*options = NULL;
		if(*port){
			//scan for space after port
			for(u = 0; (*port)[u] && !isspace((*port)[u]); u++){
			}
			if(isspace((*port)[u])){
				(*port)[u] = 0;
				*options = (*port) + u + 1;
			}
		}
	}
}

int mmbackend_parse_sockaddr(char* host, char* port, struct sockaddr_storage* addr, socklen_t* len){
	struct addrinfo* head;
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC
	};

	int error = getaddrinfo(host, port, &hints, &head);
	if(error || !head){
		LOGPF("Failed to parse address %s port %s: %s", host, port, gai_strerror(error));
		return 1;
	}

	memcpy(addr, head->ai_addr, head->ai_addrlen);
	if(len){
		*len = head->ai_addrlen;
	}

	freeaddrinfo(head);
	return 0;
}

int mmbackend_socket(char* host, char* port, int socktype, uint8_t listener, uint8_t mcast, uint8_t dualstack){
	int fd = -1, status, yes = 1;
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = socktype,
		.ai_flags = (listener ? AI_PASSIVE : 0)
	};
	struct addrinfo *info, *addr_it;

	status = getaddrinfo(host, port, &hints, &info);
	if(status){
		LOGPF("Failed to parse address %s port %s: %s", host, port, gai_strerror(status));
		return -1;
	}

	//traverse the result list
	for(addr_it = info; addr_it; addr_it = addr_it->ai_next){
		fd = socket(addr_it->ai_family, addr_it->ai_socktype, addr_it->ai_protocol);
		if(fd < 0){
			continue;
		}

		//set required socket options
		yes = 1;
		if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void*) &yes, sizeof(yes)) < 0){
			LOGPF("Failed to enable SO_REUSEADDR on socket: %s", mmbackend_socket_strerror(errno));
		}

		yes = dualstack ? 0 : 1;
		if(addr_it->ai_family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (void*) &yes, sizeof(yes)) < 0){
			LOGPF("Failed to %s dualstack operations on socket: %s", dualstack ? "enable" : "disable", mmbackend_socket_strerror(errno));
		}

		if(mcast){
			yes = 1;
			if(setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (void*) &yes, sizeof(yes)) < 0){
				LOGPF("Failed to enable SO_BROADCAST on socket: %s", mmbackend_socket_strerror(errno));
			}

			yes = 0;
			if(setsockopt(fd, addr_it->ai_family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6, addr_it->ai_family == AF_INET ? IP_MULTICAST_LOOP : IPV6_MULTICAST_LOOP, (void*) &yes, sizeof(yes)) < 0){
				LOGPF("Failed to disable IP_MULTICAST_LOOP on socket: %s", mmbackend_socket_strerror(errno));
			}
		}

		if(listener){
			status = bind(fd, addr_it->ai_addr, addr_it->ai_addrlen);
			if(status < 0){
				closesocket(fd);
				continue;
			}
		}
		else{
			status = connect(fd, addr_it->ai_addr, addr_it->ai_addrlen);
			if(status < 0){
				closesocket(fd);
				continue;
			}
		}

		break;
	}
	freeaddrinfo(info);

	if(!addr_it){
		LOGPF("Failed to create socket for %s port %s", host, port);
		return -1;
	}

	//set nonblocking
	#ifdef _WIN32
	u_long mode = 1;
	if(ioctlsocket(fd, FIONBIO, &mode) != NO_ERROR){
		closesocket(fd);
		return 1;
	}
	#else
	int flags = fcntl(fd, F_GETFL, 0);
	if(fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0){
		LOGPF("Failed to set socket nonblocking: %s", mmbackend_socket_strerror(errno));
		close(fd);
		return -1;
	}
	#endif

	return fd;
}

int mmbackend_send(int fd, uint8_t* data, size_t length){
	ssize_t total = 0, sent;
	while(total < length){
		#ifndef LIBMMBACKEND_TCP_TORTURE
		sent = send(fd, data + total, length - total, 0);
		#else
		sent = send(fd, data + total, 1, 0);
		#endif
		if(sent < 0){
			LOGPF("Failed to send: %s", mmbackend_socket_strerror(errno));
			return 1;
		}
		total += sent;
	}
	return 0;
}

int mmbackend_send_str(int fd, char* data){
	return mmbackend_send(fd, (uint8_t*) data, strlen(data));
}

json_type json_identify(char* json, size_t length){
	size_t n;

	//skip leading blanks
	for(n = 0; json[n] && n < length && isspace(json[n]); n++){
	}

	if(n == length){
		return JSON_INVALID;
	}

	switch(json[n]){
		case '{':
			return JSON_OBJECT;
		case '[':
			return JSON_ARRAY;
		case '"':
			return JSON_STRING;
		case '-':
		case '+':
			return JSON_NUMBER;
		default:
			//true false null number
			if(!strncmp(json + n, "true", 4)
					|| !strncmp(json + n, "false", 5)){
				return JSON_BOOL;
			}
			else if(!strncmp(json + n, "null", 4)){
				return JSON_NULL;
			}
			//a bit simplistic but it should do
			if(isdigit(json[n])){
				return JSON_NUMBER;
			}
	}
	return JSON_INVALID;
}

size_t json_validate(char* json, size_t length){
	switch(json_identify(json, length)){
		case JSON_STRING:
			return json_validate_string(json, length);
		case JSON_ARRAY:
			return json_validate_array(json, length);
		case JSON_OBJECT:
			return json_validate_object(json, length);
		case JSON_INVALID:
			return 0;
		default:
			return json_validate_value(json, length);
	}
}

size_t json_validate_string(char* json, size_t length){
	size_t string_length = 0, offset;

	//skip leading whitespace
	for(offset = 0; json[offset] && offset < length && isspace(json[offset]); offset++){
	}

	if(offset == length || json[offset] != '"'){
		return 0;
	}

	//find terminating quotation mark not preceded by escape
	for(string_length = 1; offset + string_length < length
			&& isprint(json[offset + string_length])
			&& (json[offset + string_length] != '"' || json[offset + string_length - 1] == '\\'); string_length++){
	}

	//complete string found
	if(json[offset + string_length] == '"' && json[offset + string_length - 1] != '\\'){
		return offset + string_length + 1;
	}

	return 0;
}

size_t json_validate_array(char* json, size_t length){
	size_t offset = 0;

	//skip leading whitespace
	for(offset = 0; json[offset] && offset < length && isspace(json[offset]); offset++){
	}

	if(offset == length || json[offset] != '['){
		return 0;
	}

	for(offset++; offset < length; offset++){
		offset += json_validate(json + offset, length - offset);

		//skip trailing whitespace, find terminator
		for(; offset < length && isspace(json[offset]); offset++){
		}

		if(json[offset] == ','){
			continue;
		}

		if(json[offset] == ']'){
			return offset + 1;
		}

		break;
	}

	return 0;
}

size_t json_validate_object(char* json, size_t length){
	size_t offset = 0;

	//skip whitespace
	for(offset = 0; json[offset] && isspace(json[offset]); offset++){
	}

	if(offset == length || json[offset] != '{'){
		return 0;
	}

	for(offset++; offset < length; offset++){
		if(json_identify(json + offset, length - offset) != JSON_STRING){
			//still could be an empty object...
			for(; offset < length && isspace(json[offset]); offset++){
			}
			if(json[offset] == '}'){
				return offset + 1;
			}
			return 0;
		}
		offset += json_validate(json + offset, length - offset);

		//find value separator
		for(; offset < length && isspace(json[offset]); offset++){
		}

		if(json[offset] != ':'){
			return 0;
		}

		offset++;
		offset += json_validate(json + offset, length - offset);

		//skip trailing whitespace
		for(; json[offset] && isspace(json[offset]); offset++){
		}

		if(json[offset] == '}'){
			return offset + 1;
		}
		else if(json[offset] != ','){
			return 0;
		}
	}
	return 0;
}

size_t json_validate_value(char* json, size_t length){
	size_t offset = 0, value_length;

	//skip leading whitespace
	for(offset = 0; json[offset] && offset < length && isspace(json[offset]); offset++){
	}

	if(offset == length){
		return 0;
	}

	//match complete values
	if(length - offset >= 4 && !strncmp(json + offset, "null", 4)){
		return offset + 4;
	}
	else if(length - offset >= 4 && !strncmp(json + offset, "true", 4)){
		return offset + 4;
	}
	else if(length - offset >= 5 && !strncmp(json + offset, "false", 5)){
		return offset + 5;
	}

	if(json[offset] == '-' || isdigit(json[offset])){
		//json number parsing is dumb.
		for(value_length = 1; offset + value_length < length &&
					(isdigit(json[offset + value_length])
					|| json[offset + value_length] == '+'
					|| json[offset + value_length] == '-'
					|| json[offset + value_length] == '.'
					|| tolower(json[offset + value_length]) == 'e'); value_length++){
		}

		if(value_length > 0){
			return offset + value_length;
		}
	}

	return 0;
}

size_t json_obj_offset(char* json, char* key){
	size_t offset = 0;
	uint8_t match = 0;

	//skip whitespace
	for(offset = 0; json[offset] && isspace(json[offset]); offset++){
	}

	if(json[offset] != '{'){
		return 0;
	}
	offset++;

	while(json_identify(json + offset, strlen(json + offset)) == JSON_STRING){
		//skip to key begin
		for(; json[offset] && json[offset] != '"'; offset++){
		}

		if(!strncmp(json + offset + 1, key, strlen(key)) && json[offset + 1 + strlen(key)] == '"'){
			//key found
			match = 1;
		}

		offset += json_validate_string(json + offset, strlen(json + offset));

		//skip to value separator
		for(; json[offset] && json[offset] != ':'; offset++){
		}

		//skip whitespace
		for(offset++; json[offset] && isspace(json[offset]); offset++){
		}

		if(match){
			return offset;
		}

		//add length of value
		offset += json_validate(json + offset, strlen(json + offset));

		//skip trailing whitespace
		for(; json[offset] && isspace(json[offset]); offset++){
		}

		if(json[offset] == ','){
			offset++;
			continue;
		}

		break;
	}

	return 0;
}

size_t json_array_offset(char* json, uint64_t key){
	size_t offset = 0, index = 0;

	//skip leading whitespace
	for(offset = 0; json[offset] && isspace(json[offset]); offset++){
	}

	if(json[offset] != '['){
		return 0;
	}

	for(offset++; index <= key; offset++){
		//skip whitespace
		for(; json[offset] && isspace(json[offset]); offset++){
		}

		if(index == key){
			return offset;
		}

		offset += json_validate(json + offset, strlen(json + offset));

		//skip trailing whitespace, find terminator
		for(; json[offset] && isspace(json[offset]); offset++){
		}

		if(json[offset] != ','){
			break;
		}
		index++;
	}

	return 0;
}

json_type json_obj(char* json, char* key){
	size_t offset = json_obj_offset(json, key);
	if(offset){
		return json_identify(json + offset, strlen(json + offset));
	}
	return JSON_INVALID;
}

json_type json_array(char* json, uint64_t key){
	size_t offset = json_array_offset(json, key);
	if(offset){
		return json_identify(json + offset, strlen(json + offset));
	}
	return JSON_INVALID;
}

uint8_t json_obj_bool(char* json, char* key, uint8_t fallback){
	size_t offset = json_obj_offset(json, key);
	if(offset){
		if(!strncmp(json + offset, "true", 4)){
			return 1;
		}
		if(!strncmp(json + offset, "false", 5)){
			return 0;
		}
	}
	return fallback;
}

uint8_t json_array_bool(char* json, uint64_t key, uint8_t fallback){
	size_t offset = json_array_offset(json, key);
	if(offset){
		if(!strncmp(json + offset, "true", 4)){
			return 1;
		}
		if(!strncmp(json + offset, "false", 5)){
			return 0;
		}
	}
	return fallback;
}

int64_t json_obj_int(char* json, char* key, int64_t fallback){
	char* next_token = NULL;
	int64_t result;
	size_t offset = json_obj_offset(json, key);
	if(offset){
		result = strtol(json + offset, &next_token, 10);
		if(next_token != json + offset){
			return result;
		}
	}
	return fallback;
}

double json_obj_double(char* json, char* key, double fallback){
	char* next_token = NULL;
	double result;
	size_t offset = json_obj_offset(json, key);
	if(offset){
		result = strtod(json + offset, &next_token);
		if(next_token != json + offset){
			return result;
		}
	}
	return fallback;
}

int64_t json_array_int(char* json, uint64_t key, int64_t fallback){
	char* next_token = NULL;
	int64_t result;
	size_t offset = json_array_offset(json, key);
	if(offset){
		result = strtol(json + offset, &next_token, 10);
		if(next_token != json + offset){
			return result;
		}
	}
	return fallback;
}

double json_array_double(char* json, uint64_t key, double fallback){
	char* next_token = NULL;
	double result;
	size_t offset = json_array_offset(json, key);
	if(offset){
		result = strtod(json + offset, &next_token);
		if(next_token != json + offset){
			return result;
		}
	}
	return fallback;
}

char* json_obj_str(char* json, char* key, size_t* length){
	size_t offset = json_obj_offset(json, key), raw_length;
	if(offset){
		raw_length = json_validate_string(json + offset, strlen(json + offset));
		if(length){
			*length = raw_length - 2;
		}
		return json + offset + 1;
	}
	return NULL;
}

char* json_obj_strdup(char* json, char* key){
	size_t len = 0;
	char* value = json_obj_str(json, key, &len), *rv = NULL;
	if(len){
		rv = calloc(len + 1, sizeof(char));
		if(rv){
			memcpy(rv, value, len);
		}
	}
	return rv;
}

char* json_array_str(char* json, uint64_t key, size_t* length){
	size_t offset = json_array_offset(json, key), raw_length;
	if(offset){
		raw_length = json_validate_string(json + offset, strlen(json + offset));
		if(length){
			*length = raw_length - 2;
		}
		return json + offset + 1;
	}
	return NULL;
}

char* json_array_strdup(char* json, uint64_t key){
	size_t len = 0;
	char* value = json_array_str(json, key, &len), *rv = NULL;
	if(len){
		rv = calloc(len + 1, sizeof(char));
		if(rv){
			memcpy(rv, value, len);
		}
	}
	return rv;
}
