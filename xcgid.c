/**
* inetd cgi wrapper
* Copyright (C) 2011-2015 Eli Cohen
* 
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along
* with this program; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include <stdio.h>
#include <string.h>
#include <unistd.h> // execl()
#include <stdlib.h> // exit()
#define MAX_HEADER 8191
#define ERRSTR "HTTP/1.1 500 Internal Server Error\r\n" \
		"Content-Type: text/plain\r\n\r\n" 

enum { R, W };
enum { false, true };

// yanked from webfsd
const struct HTTP_STATUS {
    int   status;
    char *head;
    char *body;
} http[] = {
    { 200, "200 OK",                       NULL },
    { 206, "206 Partial Content",          NULL },
    { 304, "304 Not Modified",             NULL },
    { 400, "400 Bad Request",              "*PLONK*\n" },
    { 401, "401 Authentication required",  "Authentication required\n" },
    { 403, "403 Forbidden",                "Access denied\n" },
    { 404, "404 Not Found",                "File or directory not found\n" },
    { 408, "408 Request Timeout",          "Request Timeout\n" },
    { 412, "412 Precondition failed.",     "Precondition failed\n" },
    { 500, "500 Internal Server Error",    "Sorry folks\n" },
    { 501, "501 Not Implemented",          "Sorry folks\n" },
    {   0, "500 Internal Server Error",    "WTF!?\n" }
};

void mkerror(int status) {
	int i;
	for (i = 0; http[i].status != 0; i++)
		if (http[i].status == status)
			break;

	printf("HTTP/1.1 %s\r\n", http[i].head);
	printf("Content-Type: text/plain\r\n\r\n");
	printf("%s", http[i].body);
	fflush(stdout);
	exit(-1);
}

void content(char **args, char *postdata, int length) {
	int pipefd[2];
	char buf;
	int i = 0;
	
	if (postdata) {
		if (pipe(pipefd)) {
			perror("pipe");
			exit(-1);
		}
	}


	printf("HTTP/1.1 200 OK\r\n");
	// uncomment next line to serve with non cgi programs
//	printf("Content-Type: text/plain\r\n\r\n"); 
	fflush(stdout);

	switch(fork()) {
	case -1:
		perror(ERRSTR "fork");
		exit(-1);
	case 0: //child process execs cgi script
		if (postdata) {
			close(pipefd[W]);
			dup2(pipefd[R], 0);
			close(pipefd[R]);
		}
		execv(args[0], &args[1]);
		printf("Content-Type: text/plain\r\n\r\nthe cgi script failed to run.\n");
		exit(-1);
	default:
		if (postdata) {
			close(pipefd[R]);
			write(pipefd[W], postdata, length);
			free(postdata);
			close(pipefd[W]);
		}
	}
	wait(NULL);
	close(1);

	exit(0);
}

int main(int argc, char **argv) {
	char buf[MAX_HEADER + 1];
	char orig_buf[MAX_HEADER + 1];
	char *ptr, *ptr2, *tok;
	int r, i;
	char *method = NULL;
	int length = 0;
	char *postdata = NULL;
	char **args = argv;

	setenv("REMOTE_ADDR", getenv("REMOTE_HOST"), true);

	// initial request
	do {
		i = 0;
		if ((r = read(0, &orig_buf[i], MAX_HEADER - i)) < 0)
			mkerror(400);
		i += r;
		buf[i] = '\0';
	} while ((i < MAX_HEADER) && (strstr(orig_buf, "\r\n\r\n") == NULL) && (r != 0));

	if (argc < 2) {
		printf(ERRSTR "usage: %s /path/to/cgi/script args\n", args[0]);
		exit(-1);
	}

	args[0] = args[1];
	if (args[argc]) {
		printf(ERRSTR "argv lacking null terminator.\n");
		exit(-1);
	}

	strncpy(buf, orig_buf, MAX_HEADER);
		
	if (strlen(buf) == 0)
		mkerror(400);

	tok = strtok(buf, "\n");
	tok[strcspn(tok, "\r\n")] = '\0';
	if ((ptr = strpbrk(tok, "\t ")) == NULL)
		mkerror(400);
	*ptr = '\0';
	method = strdup(buf);
	setenv("REQUEST_METHOD", method, true);
	ptr += strspn(++ptr, "\t ");
	if ((ptr2 = strpbrk(ptr, "\t ")) == NULL)
		mkerror(400);
	*ptr2 = '\0';
	setenv("REQUEST_URI", ptr, true);

	// the request headers
	for(;;) {
		tok = strtok(NULL, "\n");
		if (tok == NULL)
			break;
		tok[strcspn(tok, "\r\n")] = '\0';
		if (strlen(tok) == 0) {
			if (length && (strcasecmp(method, "POST") == 0)) {
				postdata = malloc(length + 1); 
				if (!postdata)
					mkerror(500);
				i = 0;
				tok = strtok(NULL, "\r\n");
				while ((i < length) && tok && tok[i]) {
					postdata[i] = tok[i];
					i++;
				}
				postdata[i] = '\0';
			}
			content(args, postdata, length);
		}
		else if (strncasecmp(tok, "host: ", 6) == 0)
			setenv("SERVER_NAME", &tok[6], true);
		else if (strncasecmp(tok, "referer: ", 9) == 0)
			setenv("HTTP_REFERER", &tok[9], true);
		else if (strncasecmp(tok, "user-agent: ", 12) == 0)
			setenv("HTTP_USER_AGENT", &tok[12], true);
		else if (strncasecmp(tok, "content-length: ", 16) == 0) {
			length = atoi(strdup(&tok[16]));
			setenv("CONTENT_LENGTH", &tok[16], true);
			if ((ptr = strstr(orig_buf, "\r\n\r\n")) == NULL)
				mkerror(400);
			ptr += 4;
			while (((i = strlen(ptr)) < length)
			    && (read(0, &ptr[i], length - i) > 0)
			    && ((&ptr[i] - orig_buf) < MAX_HEADER));
			strncpy(buf, orig_buf, MAX_HEADER);
		} else if (strncasecmp(tok, "cookie: ", 8) == 0) {
			ptr = getenv("HTTP_COOKIE");
			snprintf(buf, MAX_HEADER, "%s%s; ", ptr ? ptr : "", &tok[8]);
			setenv("HTTP_COOKIE", buf, true);
			strncpy(buf, orig_buf, MAX_HEADER); // ehhhh...
		}
	}
	mkerror(400);
}
