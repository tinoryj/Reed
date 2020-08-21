#include "ssl.hh"

using namespace std;

extern void timerStart(double* t);
extern double timerSplit(const double* t);

extern void fatalx(char* s);

/*
 * constructor: initialize sock structure and connect
 *
 * @param ip - server ip address
 * @param port - port number
 */
Ssl::Ssl(char* ip, int port, int userID)
{

    /* get port and ip */
    hostPort_ = port;
    hostName_ = ip;
    int err;

    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    ctx_ = SSL_CTX_new(TLSv1_client_method());
    if (ctx_ == NULL)
        fatalx("ctx");

    if (!SSL_CTX_load_verify_locations(ctx_, CACRT, NULL))
        fatalx("verify");

    if (!SSL_CTX_use_certificate_file(ctx_, CLCRT, SSL_FILETYPE_PEM))
        fatalx("cert");
    if (!SSL_CTX_use_PrivateKey_file(ctx_, CLKEY, SSL_FILETYPE_PEM))
        fatalx("key");
    if (!SSL_CTX_check_private_key(ctx_))
        fatalx("cert/key");

    SSL_CTX_set_mode(ctx_, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_verify_depth(ctx_, 1);

    /* initializing socket object */
    hostSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (hostSock_ == -1) {
        printf("Error initializing socket %d\n", errno);
    }
    int* p_int = (int*)malloc(sizeof(int));
    *p_int = 1;

    /* set socket options */
    if (
        (setsockopt(hostSock_,
             SOL_SOCKET,
             SO_REUSEADDR,
             (char*)p_int,
             sizeof(int))
            == -1)
        || (setsockopt(hostSock_,
                SOL_SOCKET,
                SO_KEEPALIVE,
                (char*)p_int,
                sizeof(int))
            == -1)) {
        printf("Error setting options %d\n", errno);
        free(p_int);
    }
    free(p_int);

    /* set socket address */
    myAddr_.sin_family = AF_INET;
    myAddr_.sin_port = htons(port);
    memset(&(myAddr_.sin_zero), 0, 8);
    myAddr_.sin_addr.s_addr = inet_addr(ip);

    /* trying to connect socket */
    if (connect(hostSock_, (struct sockaddr*)&myAddr_, sizeof(myAddr_)) == -1) {
        if ((err = errno) != EINPROGRESS) {
            fprintf(stderr, "Error connecting socket %d\n", errno);
        }
    }

    ssl_ = SSL_new(ctx_);
    SSL_set_fd(ssl_, hostSock_);

    if (SSL_connect(ssl_) <= 0)
        fatalx("SSL_connect");

    if (SSL_get_verify_result(ssl_) != X509_V_OK)
        fatalx("cert");

    /* prepare user ID and send it to server */
    int netorder = htonl(userID);
    int bytecount;
    if ((bytecount = SSL_write(ssl_, &netorder, sizeof(int))) == -1) {
        fprintf(stderr, "Error sending userID %d\n", errno);
    }
}

/*
 * @ destructor
 */
Ssl::~Ssl()
{
    SSL_free(ssl_);
    SSL_CTX_free(ctx_);
    close(hostSock_);
}

void Ssl::closeConn()
{
    int last = -7;
    genericSend((char*)&last, sizeof(int));
}

/*
 * basic send function
 * 
 * @param raw - raw data buffer_
 * @param rawSize - size of raw data
 */
int Ssl::genericSend(char* raw, int rawSize)
{

    int bytecount;
    int total = 0;
    while (total < rawSize) {
        if ((bytecount = SSL_write(ssl_, raw + total, rawSize - total)) == -1) {
            fprintf(stderr, "Error sending data %d\n", errno);
            return -1;
        }
        total += bytecount;
    }
    return total;
}

/*
 *
 * @param raw - raw data buffer
 * @param rawSize - the size of data to be downloaded
 * @return raw
 */
int Ssl::genericDownload(char* raw, int rawSize)
{

    int bytecount;
    int total = 0;
    while (total < rawSize) {
        if ((bytecount = SSL_read(ssl_, raw + total, rawSize - total)) == -1) {
            fprintf(stderr, "Error sending data %d\n", errno);
            return -1;
        }
        total += bytecount;
    }
    return 0;
}

/*
 * initiate downloading a file
 *
 * @param filename - the full name of the targeting file
 * @param namesize - the size of the file path
 *
 *
 */
int Ssl::initDownload(char* filename, int namesize)
{

    int indicator = INIT_DOWNLOAD;

    memcpy(buffer_, &indicator, sizeof(int));
    memcpy(buffer_ + sizeof(int), &namesize, sizeof(int));
    memcpy(buffer_ + 2 * sizeof(int), filename, namesize);
    genericSend(buffer_, sizeof(int) * 2 + namesize);

    return 0;
}

/*
 * download a chunk of data
 *
 * @param raw - the returned raw data chunk
 * @param retSize - the size of returned data chunk
 * @return raw 
 * @return retSize
 */
int Ssl::downloadChunk(char* raw, int* retSize)
{

    int bytecount;

    char* buffer = (char*)malloc(sizeof(char) * SOCKET_BUFFER_SIZE);
    if ((bytecount = SSL_read(ssl_, buffer, sizeof(int))) == -1) {

        fprintf(stderr, "Error receiving data %d\n", errno);
    }
    if ((bytecount = SSL_read(ssl_, buffer, sizeof(int))) == -1) {

        fprintf(stderr, "Error receiving data %d\n", errno);
        return -1;
    }
    *retSize = *(int*)buffer;

    genericDownload(raw, *retSize);
    return 0;
}
