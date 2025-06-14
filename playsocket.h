#pragma once

#include <cstdint>
#include <stdlib.h>
#include <unistd.h>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>

enum MessageType {
    MSG_FAILED,
    MSG_TYPE_DATA,
    MSG_TYPE_DATA_NEEDS_REPLY,
	MSG_TYPE_DATA_REPLY,
    MSG_TYPE_FD
};

struct MessageHeader {
    uint32_t type;   // from MessageType
    uint32_t length; // length of the payload (excluding header)
};

enum DataType {
    MSG_HELLO,
    MSG_ASK_FOR_RESOLUTION,
    MSG_HAVE_RESOLUTION,
    MSG_HAVE_BUFFER
};

struct MessageData {
	DataType type;
	int width;
	int height;
	int refresh_rate;

    int format;
    uint64_t modifiers;
    int32_t stride;
    int32_t offset;
};

int create_socket(const char *path) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    unlink(path);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		exit(-1);
	}
    listen(sock, 1);

    return accept(sock, NULL, NULL);
}

int connect_socket(const char *path) {
    struct sockaddr_un addr;
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    connect(sock, (struct sockaddr *)&addr, sizeof(addr));

	return sock;
}

int send_message(int sock, int fd, MessageType type, MessageData *payload) {
    struct msghdr msg;
	memset(&msg, 0, sizeof(msg));

	size_t payload_len = payload ? sizeof(MessageData) : 0;
	if (payload && payload_len > sizeof(MessageData)) {
		fprintf(stderr, "Payload length exceeds maximum size\n");
		return -1;
	}

    // Construct message header
    MessageHeader header = {(uint32_t)type, (uint32_t)payload_len};

    // Compose the full payload: header + actual data
    std::vector<char> buffer(sizeof(header) + payload_len);
    memcpy(buffer.data(), &header, sizeof(header));
    if (payload && payload_len > 0) {
        memcpy(buffer.data() + sizeof(header), payload, payload_len);
    }

    struct iovec io;
    io.iov_base = buffer.data();
    io.iov_len = buffer.size();

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    char control_buf[CMSG_SPACE(sizeof(int))] = {0};
    if (type == MSG_TYPE_FD) {
        msg.msg_control = control_buf;
        msg.msg_controllen = sizeof(control_buf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(fd));

        memmove(CMSG_DATA(cmsg), &fd, sizeof(fd));
    }

    if (sendmsg(sock, &msg, 0) < 0) {
        fprintf(stderr, "sendmsg failed\n");
        return -1;
    }

	return 0;
}

int recv_message(int sock, int *fd_out, MessageData *buffer, MessageType* out_type) {
    struct msghdr msg;
    struct iovec io;
    char control_buf[256] = {0};

    size_t buffer_size = buffer ? sizeof(MessageData) : 0;
    if (buffer && buffer_size > sizeof(MessageData)) {
		fprintf(stderr, "Payload length exceeds maximum size\n");
		return -1;
	}

    memset(&msg, 0, sizeof(msg));

    // Expecting header + payload
    std::vector<char> recv_buf(sizeof(MessageHeader) + buffer_size);
    io.iov_base = recv_buf.data();
    io.iov_len = recv_buf.size();

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    msg.msg_control = control_buf;
    msg.msg_controllen = sizeof(control_buf);

    ssize_t n = recvmsg(sock, &msg, 0);
	if (n == 0) {
		*out_type = MSG_FAILED;
		return 0; // Connection closed
	}
    if (n < 0) {
        fprintf(stderr, "recvmsg failed\n");
        *out_type = MSG_FAILED;
        return n;
    }

    if ((uint32_t)n < sizeof(MessageHeader)) {
        fprintf(stderr, "recvmsg received less than header size\n");
        *out_type = MSG_FAILED;
        return -1;
    }

    MessageHeader header;
    memcpy(&header, recv_buf.data(), sizeof(header));
    if (header.length > buffer_size) {
		fprintf(stderr, "recvmsg received data larger than buffer size\n");
		*out_type = MSG_FAILED;
        return -1;
    }

    if (header.length > 0 && buffer) {
        memcpy(buffer, recv_buf.data() + sizeof(header), header.length);
    }

    if (header.type == MSG_TYPE_FD && fd_out) {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            memmove(fd_out, CMSG_DATA(cmsg), sizeof(int));
        } else {
            *fd_out = -1;
        }
    }

	*out_type = (MessageType)header.type;
    return n;
}
