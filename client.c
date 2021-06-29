#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#define RESPONSE_BUFFER_SIZE 1024 * 1024 * 100

typedef struct
{
    char *headers;
    char *body;
    int bodyLength;
} Response;

char *base64_encode(char *input, int input_length)
{
    char table[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
                    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
                    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
                    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
                    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
                    'w', 'x', 'y', 'z', '0', '1', '2', '3',
                    '4', '5', '6', '7', '8', '9', '+', '/'};
    int output_length = 4 * ((input_length + 2) / 3);

    char *result = malloc(output_length + 1);

    int i = 0, j = 0;
    while (i < input_length)
    {
        uint32_t octet_a = i < input_length ? input[i++] : 0;
        uint32_t octet_b = i < input_length ? input[i++] : 0;
        uint32_t octet_c = i < input_length ? input[i++] : 0;

        uint32_t combined = (octet_a << 16) + (octet_b << 8) + (octet_c);

        result[j++] = table[(combined >> 18) & 63];
        result[j++] = table[(combined >> 12) & 63];
        result[j++] = table[(combined >> 6) & 63];
        result[j++] = table[combined & 63];
    }

    if (input_length % 3 != 0)
    {
        for (int i = 0; i < (3 - (input_length % 3)); i++)
        {
            result[output_length - 1 - i] = '=';
        }
    }

    result[output_length] = '\0';
    return result;
}

void parse_url(char *url, char **protocol, char **host, char **port, char **path)
{
    /*
    protocol, host, port, path and hash are valid components of an url
    host is the only required component
    Note that leading slash in path gets discarded

    Parsing logic:
        Replace delimiters ('://', ':', '/', '#') between components with null character('\0)
        string gets split into multiple strings, which are the desired components
    */

    char *helper; // pointer to help with finding & replacing delimiters

    *protocol = "http";
    helper = strstr(url, "://");
    if (helper)
    {
        *helper = '\0';
        helper += 3;
        *protocol = url;
    }
    else
    {
        helper = url;
    }

    *host = helper;
    while (*helper && *helper != ':' && *helper != '/' && *helper != '#')
        ++helper;

    *port = "80";
    if (*helper == ':')
    {
        *helper = '\0';
        ++helper;
        *port = helper;
    }
    while (*helper && *helper != '/' && *helper != '#')
        ++helper;

    *path = "";
    if (*helper == '/')
    {
        *helper = '\0';
        ++helper;
        *path = helper;
    }
    while (*helper && *helper != '#')
        ++helper;

    // discard hash if any
    if (*helper == '#')
    {
        *helper = '\0';
    }
}

int connect_host(char *host, char *port)
{
    int server;
    struct addrinfo hints, *head, *current;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(host, port, &hints, &head);
    if (status)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        exit(EXIT_FAILURE);
    }

    /*
    getaddrinfo() returns a list of address structures
    Try each address until we successfully connect
    */
    for (current = head; current != NULL; current = current->ai_next)
    {
        server = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (server == -1)
            continue;

        if (connect(server, current->ai_addr, current->ai_addrlen) != -1)
            break;

        close(server); //close socket which could not connect
    }
    freeaddrinfo(head);

    if (current == NULL)
    {
        fprintf(stderr, "Could not connect\n");
        exit(EXIT_FAILURE);
    }

    return server;
}

/*
    body length of HTTP response is determined by:
    if present, 'Content-Length' Header
    else,
        if 'Transfer-Encoding: chunked' Header line found,
            response body will be sent in separate chunks
            Each chunk begins with its chunk length encoded in hexadecimal,
            followed by a newline and then the chunk data
            end of response body is denoted by a zero length chunk
        else,
            Server closes TCP connection after sending the body
            Thus end of connection denotes end of body
*/
// TODO: handle Transfer-Encoding: chunked
void receive_response(int socket, Response *result)
{
    char *response = calloc(RESPONSE_BUFFER_SIZE, sizeof(char));
    char *filled = response, *end = response + RESPONSE_BUFFER_SIZE;
    char *headers = NULL, *body = NULL, *temp = NULL;
    enum
    {
        length,
        connection
    };
    int encoding = connection;
    int remaining = 0;

    while (1)
    {
        if (filled == end)
        {
            fprintf(stderr, "Out of buffer space while receiving response\n");
            exit(EXIT_FAILURE);
        }

        int bytes_received = read(socket, filled, end - filled);

        if (bytes_received == -1)
        {
            perror("read");
            exit(EXIT_FAILURE);
        }

        if (bytes_received == 0)
        {
            if (encoding == connection && body)
            {
                result->headers = headers;
                result->body = body;
                result->bodyLength = (int)(filled - body);
                break;
            }
            else
            {
                fprintf(stderr, "Server closed connection unexpectedly\n");
                exit(EXIT_FAILURE);
            }
        }

        filled += bytes_received;

        if (!body)
        {
            *filled = '\0'; // will be overwritten by next read()
            temp = strstr(response, "\r\n\r\n");
            if (temp)
            {
                *temp = '\0';
                temp += 4;
                body = temp;
                headers = response;

                temp = strstr(headers, "\nContent-Length: ");
                if (temp)
                {
                    encoding = length;
                    temp = strstr(temp, " ");
                    ++temp;
                    remaining = strtol(temp, NULL, 10);
                }
                else
                {
                    encoding = connection;
                }
            }
        }

        if (body)
        {
            if (encoding == length)
            {
                if ((int)(filled - body) >= remaining)
                {
                    result->headers = headers;
                    result->body = body;
                    result->bodyLength = remaining;
                    break;
                }
            }
        }
    }
}

Response get(char *host, char *port, char *path, char *proxy_ip, char *proxy_port, char *proxy_username, char *proxy_password)
{
    char *credentials = calloc(strlen(proxy_username) + 1 + strlen(proxy_password) + 1, sizeof(char));
    char *temp = credentials;
    strcat(credentials, proxy_username);
    temp += strlen(proxy_username);
    strcat(temp, ":");
    ++temp;
    strcat(temp, proxy_password);
    char *credentials_base64 = base64_encode(credentials, strlen(credentials));

    int server = connect_host(proxy_ip, proxy_port);

    char buffer[2048];
    sprintf(buffer, "GET http://%s/%s HTTP/1.1\r\n", host, path);
    sprintf(buffer + strlen(buffer), "Host: %s:%s\r\n", host, port);
    sprintf(buffer + strlen(buffer), "Proxy-Authorization: Basic %s\r\n", credentials_base64);
    sprintf(buffer + strlen(buffer), "Connection: close\r\n");
    sprintf(buffer + strlen(buffer), "\r\n");
    printf("\nRequest Headers:\n%s\n", buffer);

    write(server, buffer, strlen(buffer));

    Response response;
    receive_response(server, &response);
    printf("\nResponse Headers\n%s\n", response.headers);

    close(server);
    return response;
}

int main(int argc, char *argv[])
{
    if (argc != 8)
    {
        fprintf(stderr, "Unexpected number of arguments specified\n");
        fprintf(stderr, "Usage: %s url proxy_ip proxy_port proxy_username proxy_pass html_path logo_path\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char *url = argv[1], *proxy_ip = argv[2], *proxy_port = argv[3], *proxy_username = argv[4], *proxy_password = argv[5], *html_path = argv[6], *logo_path = argv[7];

    char *protocol, *host, *port, *path;
    Response response_html;
    while (1)
    {

        parse_url(url, &protocol, &host, &port, &path);

        if (protocol)
        {
            if (strcmp(protocol, "http"))
            {
                fprintf(stderr, "Unsupported protocol (%s) specified\n", protocol);
                fprintf(stderr, "Only HTTP is supported\n");
                exit(EXIT_FAILURE);
            }
        }

        response_html = get(host, port, path, proxy_ip, proxy_port, proxy_username, proxy_password);
        char *temp = strstr(response_html.headers, "HTTP/1.1 200 OK");
        if (temp)
        {
            break;
        }
        else
        {
            temp = strstr(response_html.headers, "\nLocation: ");
            if (temp)
            {
                temp = strstr(temp, " ");
                ++temp;
                char *end = strstr(temp, "\r\n");
                size_t len = end - temp;
                char *redirectLink = malloc(sizeof(char) * (len + 1));
                strncpy(redirectLink, temp, len);
                redirectLink[len] = '\0';
                url = redirectLink;
                continue;
            }
            else
            {
                fprintf(stderr, "GET request to server failed\n");
                exit(EXIT_FAILURE);
            }
        }
    }
    FILE *fptr = fopen(html_path, "w");
    if (!fptr)
    {
        perror("fopen");
        exit(EXIT_FAILURE);
    }
    fwrite(response_html.body, response_html.bodyLength, 1, fptr);
    fclose(fptr);
    free(response_html.headers);

    if (strcmp(host, "info.in2p3.fr") == 0)
    {
        Response response_logo = get(host, port, "cc.gif", proxy_ip, proxy_port, proxy_username, proxy_password);
        FILE *fptr = fopen(logo_path, "w");
        if (!fptr)
        {
            perror("fopen");
            exit(EXIT_FAILURE);
        }
        fwrite(response_logo.body, response_logo.bodyLength, 1, fptr);
        fclose(fptr);
        free(response_logo.headers);
    }

    return 0;
}