/*
 * keyserver.cc
 */

#include "keyserver.hh"
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <string.h>
#include <string>
#include <sys/time.h>

using namespace std;

void fatalx(char* s)
{

    ERR_print_errors_fp(stderr);
    errx(EX_DATAERR, "%.30s", s);
}

/*
 * constructor: initialize host socket
 *
 * @param port - port number
 * @param dedupObj - dedup object passed in
 */
KeyServer::KeyServer(int port)
{

    //server port
    hostPort_ = port;
    //initiate ssl functions
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    // create TSL connection
    ctx_ = SSL_CTX_new(TLS_server_method());
    if (ctx_ == NULL)
        fatalx("ctx");
    //load client certificate
    if (!SSL_CTX_load_verify_locations(ctx_, CACRT, NULL))
        fatalx("verify");
    SSL_CTX_set_client_CA_list(ctx_, SSL_load_client_CA_file(CACRT));
    if (!SSL_CTX_use_certificate_file(ctx_, SECRT, SSL_FILETYPE_PEM))
        fatalx("cert");
    //Load server key file
    if (!SSL_CTX_use_PrivateKey_file(ctx_, SEKEY, SSL_FILETYPE_PEM))
        fatalx("key");
    //check server key
    if (!SSL_CTX_check_private_key(ctx_))
        fatalx("cert/key");
    //init SSL connection
    SSL_CTX_set_mode(ctx_, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    SSL_CTX_set_verify_depth(ctx_, 1);
    //server socket initialization
    hostSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (hostSock_ == -1) {

        printf("Error initializing socket %d\n", errno);
    }
    //set socket options
    int* p_int = (int*)malloc(sizeof(int));
    *p_int = 1;

    if ((setsockopt(hostSock_, SOL_SOCKET, SO_REUSEADDR, (char*)p_int, sizeof(int)) == -1) || (setsockopt(hostSock_, SOL_SOCKET, SO_KEEPALIVE, (char*)p_int, sizeof(int)) == -1)) {

        printf("Error setting options %d\n", errno);
        free(p_int);
    }
    free(p_int);
    //initialize address struct
    myAddr_.sin_family = AF_INET;
    myAddr_.sin_port = htons(hostPort_);
    memset(&(myAddr_.sin_zero), 0, 8);
    myAddr_.sin_addr.s_addr = INADDR_ANY;
    //bind port
    if (bind(hostSock_, (sockaddr*)&myAddr_, sizeof(myAddr_)) == -1) {

        fprintf(stderr, "Error binding to socket %d\n", errno);
    }
    //start to listen
    if (listen(hostSock_, 10) == -1) {

        fprintf(stderr, "Error listening %d\n", errno);
    }
}

/*
 * Timer functions
 */
void timerStart(double* t)
{

    struct timeval tv;
    gettimeofday(&tv, NULL);
    *t = (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
}

double timerSplit(const double* t)
{

    struct timeval tv;
    double cur_t;
    gettimeofday(&tv, NULL);
    cur_t = (double)tv.tv_sec + (double)tv.tv_usec * 1e-6;
    return (cur_t - *t);
}

/*
 * Thread function: each thread maintains a socket from a certain client
 *
 * @param lp - input parameter structure
 */
void* SocketHandler(void* lp)
{

    //double timer,split,bw;
    //get socket from input param
    SSL* ssl = ((KeyServer*)lp)->ssl_;
    //variable initialization
    int bytecount;
    char* buffer = (char*)malloc(sizeof(char) * BUFFER_SIZE + sizeof(int));
    char* output = (char*)malloc(sizeof(char) * BUFFER_SIZE + sizeof(int));
    if ((bytecount = SSL_read(ssl, buffer, sizeof(int))) == -1) {

        fprintf(stderr, "Error recv userID %d\n", errno);
    }
    int user = ntohl(*(int*)buffer);
    printf("connection from user %d\n", user);
    // RSA structure
    RSA* rsa = RSA_new();
    BIO* key = BIO_new_file("./keys/private.pem", "r");
    PEM_read_bio_RSAPrivateKey(key, &rsa, NULL, NULL);
    BIGNUM *bnn, *bne, *bnd;
    bnn = BN_new();
    bne = BN_new();
    bnd = BN_new();
    RSA_set0_key(rsa, bnn, bne, bnd);
    // read the server private key
    while (true) {

        // Big number init
        BN_CTX* ctx = BN_CTX_new();
        BIGNUM* ret = BN_new();
        // recv data size
        if ((bytecount = SSL_read(ssl, buffer, sizeof(int))) == -1) {

            fprintf(stderr, "Error recv file hash %d\n", errno);
        }
        if (bytecount == 0) {

            BN_CTX_free(ctx);
            BN_clear_free(ret);
            break;
        }
        // prepare to recv data
        int num, total;
        memcpy(&num, buffer, sizeof(int));
        total = 0;
        // recv data (blinded hash, 1024bits values)
        while (total < num * RSA_LENGTH) {

            if ((bytecount = SSL_read(ssl, buffer + sizeof(int) + total, num * RSA_LENGTH - total)) == -1) {
                fprintf(stderr, "Error recv file hash %d\n", errno);
                exit(1);
            }
            total += bytecount;
        }

        // main loop for computing keys
        double timer, split;
        timerStart(&timer);
        for (int i = 0; i < num; i++) {

            // hash x r^e to BN
            BN_bin2bn((unsigned char*)(buffer + sizeof(int) + i * RSA_LENGTH), RSA_LENGTH, ret);
            // compute (Hash x r^e)^d mod n
            BN_mod_exp(ret, ret, bnd, bnn, ctx);
            memset(output + sizeof(int) + i * RSA_LENGTH, 0, RSA_LENGTH);
            BN_bn2bin(ret, (unsigned char*)output + sizeof(int) + i * RSA_LENGTH + (RSA_LENGTH - BN_num_bytes(ret)));
            //BN_bn2bin(ret,(unsigned char*)output+sizeof(int)+i*32);
        }
        split = timerSplit(&timer);
        printf("server compute: %lf\n", split);
        // send back the result
        total = 0;
        while (total < num * RSA_LENGTH) {

            if ((bytecount = SSL_write(ssl, output + sizeof(int) + total, num * RSA_LENGTH - total)) == -1) {

                fprintf(stderr, "Error recv file hash %d\n", errno);
                exit(1);
            }
            total += bytecount;
        }
    }
    //clean up
    BIO_free_all(key);
    RSA_free(rsa);
    free(buffer);
    free(output);
    return 0;
}

/*
 * main procedure for receiving data
 */
void KeyServer::runReceive()
{

    addrSize_ = sizeof(sockaddr_in);
    //create a thread whenever a client connects
    while (true) {

        printf("waiting for a connection\n");
        clientSock_ = (int*)malloc(sizeof(int));

        if ((*clientSock_ = accept(hostSock_, (sockaddr*)&sadr_, &addrSize_)) != -1) {

            printf("Received connection from %s\n", inet_ntoa(sadr_.sin_addr));
            // SSL verify
            ssl_ = SSL_new(ctx_);
            SSL_set_fd(ssl_, *clientSock_);
            int r;
            if ((r = SSL_accept(ssl_)) == -1)
                warn("SSL_accept");
            pthread_create(&threadId_, 0, &SocketHandler, (void*)this);
            pthread_detach(threadId_);
        } else {

            fprintf(stderr, "Error accepting %d\n", errno);
        }
    }
}

KeyServer::~KeyServer()
{

    SSL_free(ssl_);
    SSL_CTX_free(ctx_);
}
