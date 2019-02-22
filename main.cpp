#include <iostream>
#include <fstream>
#include <string> 
#include <sstream>
#include <algorithm>

#include <boost/asio.hpp>
#include <glog/logging.h>
#include <openssl/md5.h>

using namespace std;
using namespace boost::asio;

DEFINE_int32(
    port, 6110,
    "Port number for server / where server is running");

DEFINE_string(
    log, "pa2.log",
    "Specifies log file for network logging / where server log is stored");
//BTW what's up with the format for 3rd param?
DEFINE_string(
    pid, "pa2.pid",
    "Process ID file / process ID number of the server running");

DEFINE_string(
    root, "home",
    "Server root directory / where file system begins");

// value used as delimiter / for marking the end of a message
const std::string kDelimiter = "\n";
// global lookup table for file extension-MIME type/subtype match 
map<string, string> typeLookup;


int parseHTTP(string msg, string& uribuffer);
string parseString(string& buff, char delim);

void runServer();
void createMIMEtable(string mimetype_arg);
string getMD5(int off, int len, string filename);

string readUntilDelimiter(
    boost::asio::ip::tcp::socket& socket,
    boost::asio::streambuf& readBuffer);
void sendWithDelimiter(
    boost::asio::ip::tcp::socket& socket,
    const std::string& message);

void sendNotFound(
      boost::asio::ip::tcp::socket& socket);
void sendMethodNotAllowed(
      boost::asio::ip::tcp::socket& socket);
void sendBadRequest(
      boost::asio::ip::tcp::socket& socket);
void sendNormalRes(
      boost::asio::ip::tcp::socket& socket,
      string& uri
      );
void sendBytes(
    boost::asio::ip::tcp::socket& socket,
    const string& message,
    int n);

int main(int argc, char *argv[]) {
  //Variable declarations
  fstream pid_f;
  string mimetype_arg, rootdir;
  int pid;

  FLAGS_logtostderr = true;
  
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);



  LOG(INFO) << "Started pa2";
  // verify port number set
  if (FLAGS_port > 65535 || FLAGS_port < 1024) {
    LOG(FATAL) << "Invalid port number";
  }
  

  if(FLAGS_root == "home"){

    char* temp = get_current_dir_name();
    rootdir = temp;
    free(temp);
    FLAGS_root = rootdir;
  }else{

    if(FLAGS_root[0] != '/'){
      char* temp =get_current_dir_name();
      rootdir = temp;
      free(temp);
      FLAGS_root = rootdir + "/" +  FLAGS_root + "/";

    }
  }

  chdir(FLAGS_root.c_str());
  
 // verify MIME type file command 
  if(argv[argc-1] == NULL){
    LOG(FATAL) << "ERROR: No MIME type file";
  }else{
    mimetype_arg = argv[argc-1];
  }
  
  //PID
  pid = (int)getpid();

 

  //Output pid file
    pid_f.open(FLAGS_pid, fstream::out);
    pid_f << pid;
    pid_f.close();


  //Print for debug purposes
  LOG(INFO) << "INITIAL INFO";
  LOG(INFO) << "=====================";
  LOG(INFO) << "PID file: " << FLAGS_pid;
  LOG(INFO) << "LOG file: " << FLAGS_log;
  LOG(INFO) << "PORT number: " << FLAGS_port;
  LOG(INFO) << "ROOTDIR: " << FLAGS_root;
  LOG(INFO) << "MIME type file name: " << mimetype_arg;
  LOG(INFO) << "PID: " << pid;
  LOG(INFO) << "=====================";
 

  //Mime type file parsing 
  createMIMEtable(mimetype_arg);
  runServer();
  
  return 0;
}





void runServer() {
  io_service ioService;
  ip::tcp::acceptor acceptor(
      ioService, ip::tcp::endpoint(ip::tcp::v4(), FLAGS_port));
 
  fstream log_f;
  log_f.open(FLAGS_log, fstream::out);

  for (;;) {
    // setup a socket and wait on a client connection
    ip::tcp::socket socket(ioService);
    LOG(INFO) << "Waiting for client to connect";
    acceptor.accept(socket);

    // log the address of the remote client
    const auto remoteEndpoint = socket.remote_endpoint();
    LOG(INFO)
        << "Connected to client ("
        << remoteEndpoint.address() << ":" << remoteEndpoint.port() << ")";

    // wait for a message from the client
    LOG(INFO) << "Waiting for message from client";
    boost::asio::streambuf rcvBuffer;

    

    //Parse first line 
    string message = readUntilDelimiter(socket, rcvBuffer);
    string uri = "";
   
    int res_code = parseHTTP(message, uri);
    switch(res_code){
      case 200: 
        sendNormalRes(socket, uri);
        break;

      case 404:
        sendNotFound(socket);
        break;
      
      case 405:
        sendMethodNotAllowed(socket);
        break;
      
      case 400: 
        sendBadRequest(socket);
        break;
      
      default: 
        sendNotFound(socket);
        break;
    }

    // reverse the message and send it back
    // we're done
    LOG(INFO) << "Disconnected client";
  }

}

void sendNormalRes(
      boost::asio::ip::tcp::socket& socket,
      string& uri
      ){
    
    string inputFileBuf, ext, types, md5hex, len, off, filename;
    map<string, string>::iterator it; 
    int  pos;
    
    //Parse .ext
    ext = parseString(uri, '?');
    pos = ext.find('.');
    ext = ext.substr( pos + 1, ext.length() - pos );

    it = typeLookup.find(ext);

    if(it != typeLookup.end()){
      types = it->second;
    }else{
      types = "application/octet-stream";
    }

    

    //Parse length
    len = uri.substr(uri.find("?") + 1 , uri.find("&") - uri.find("?") - 1 );
    len = len.substr(len.find("=") + 1, len.length());
    
    if(len.length() == 0 || stoi(len) <= 0 ){
     sendNotFound(socket);
     return;
    }

    //Parse offset 
    off =  uri.substr(uri.find("&") + 1 , uri.length());
    off =  off.substr(off.find("=") + 1, off.length());
    
    if(off.length() == 0 || stoi(off) < 0 /* || stoi(off) > filesize*/) {
      sendNotFound(socket);
      return;
    }

    //Parse filename
    filename = parseString(uri, '?');
    
    //Open file 
    ifstream inputFile(filename, ios::in | ios::binary);
       
    if (inputFile.is_open()) {
      LOG(INFO) << "Opened file \"" << filename << "\"";



      inputFileBuf = string(
          istreambuf_iterator<char>( inputFile.seekg(stoi(off), inputFile.beg)),
          istreambuf_iterator<char>());
   
    } else {
      LOG(INFO) << "Unable to open file \"" << filename << "\"";
      sendNotFound(socket);
      return;
    }

    inputFile.close();

    //Find MD5

   

    //Create header 
    string message = "HTTP/1.1 200 OK\r\n";
      message = message + "Connection: close\r\n" +
      "Server: pa2 (jiminshi@usc.edu)\r\n" + 
      "Content-Type: " +  types + "\r\n" + 
      "Content-Length: " + len +"\r\n" + 
      "Content-MD5: " + md5hex + "\r\n";

    sendWithDelimiter(socket, message);
    sendBytes(socket, inputFileBuf, stoi(len));
}



void sendNotFound(
      boost::asio::ip::tcp::socket& socket){

      string message = "HTTP/1.1 404 Not Found\r\n";

      message = message + "Connection: close\r\n" +
      "Content-Type: text/html\r\n" + 
      "Content-Length: 63\r\n" + 
      "\r\n"+  
      "<html><head></head><body><h1>404 Not Found</h1></body></html>\r\n";
      
      boost::system::error_code error;
     
      write(socket, buffer(message), transfer_all(), error);
      if (error) {
        LOG(FATAL)
            << "Send error: "
            << boost::system::system_error(error).what();
      }
}

void sendMethodNotAllowed(
      boost::asio::ip::tcp::socket& socket){
      string message = "HTTP/1.1 405 Method Not Allowed\r\n";
      
      message = message + "Connection: close\r\n" +
      "Content-Type: text/html\r\n" + 
      "Content-Length: 72\r\n" + 
      "\r\n"+  
      "<html><head></head><body><h1>405 Method Not Allowed</h1></body></html>\r\n";
            boost::system::error_code error;
     
      write(socket, buffer(message), transfer_all(), error);
      if (error) {
        LOG(FATAL)
            << "Send error: "
            << boost::system::system_error(error).what();
      }

}

void sendBadRequest(
      boost::asio::ip::tcp::socket& socket){
      string message = "HTTP/1.1 400 Bad Request\r\n";
      
      message = message + "Connection: close\r\n" +
      "Content-Type: text/html\r\n" + 
      "Content-Length: 65\r\n" + 
      "\r\n"+  
      "<html><head></head><body><h1>400 Bad Request</h1></body></html>\r\n";
      
      boost::system::error_code error;
     
      write(socket, buffer(message), transfer_all(), error);
      if (error) {
        LOG(FATAL)
            << "Send error: "
            << boost::system::system_error(error).what();
      }
}


int parseHTTP(string msg, string& uribuffer){

  if(msg[strlen(msg.c_str())-1] == '\r'){
  	
    stringstream ss(msg);
    string method, version, uri;
    ss >> method >> uri >> version;
    
    //Check arguments
    string check;
    if(ss >> check){
      LOG(INFO) << "TRAILING MSG";
      return 400;
    }

    //Check HTTP Method
    if(method != "GET"){
      LOG(INFO) << "NOT GET METHOD";
      return 405;
    }

    //Check version
    if(version.find("HTTP/") != 0){
      LOG(INFO) << "WRONG VER";
      return 400;
    }


    //Check URI syntax
    if( uri.find("..") != string::npos ||
      count(uri.begin(), uri.end(), '?') !=1 ||
      count(uri.begin(), uri.end(), '=') !=2 ||
      count(uri.begin(), uri.end(), '&') !=1 
      ){
      LOG(INFO) << "INVALID ACCESS:" << uri;
      return 404;
    }
    if(uri[0] == '/'){
       uri.erase(0, 1);
      uribuffer = uri;
      string fpath = FLAGS_root + "/" + parseString(uri,'?');
     
      FILE* temp = fopen(fpath.c_str(), "r");
       LOG(INFO) << fpath;
      if(temp){
        fclose(temp);
        return 200;
      }else{
        return 404;
      }
    }
    

  }else{
    LOG(INFO) << "CRLF ERROR";
    return 400;
  
  }

    return 200;
    
}

void sendBytes(
    boost::asio::ip::tcp::socket& socket,
    const string& message, int n) {
  boost::system::error_code error;
  write(socket, buffer(message), transfer_exactly(n), error);
  if (error) {
    LOG(FATAL)
        << "Send error: "
        << boost::system::system_error(error).what();
  }
}



string parseString(string& str, char delim){
  int pos = str.find(delim);
  string result = str.substr(0, pos);
  return result;

}

void sendWithDelimiter(
    boost::asio::ip::tcp::socket& socket,
    const std::string& message) {
  boost::system::error_code error;
  const std::string messageWithDelimiter = message + kDelimiter;
  write(socket, buffer(messageWithDelimiter), transfer_all(), error);
  if (error) {
    LOG(FATAL)
        << "Send error: "
        << boost::system::system_error(error).what();
  }
}

string readUntilDelimiter(
    boost::asio::ip::tcp::socket& socket,
    boost::asio::streambuf& readBuffer) {
  // blocking read on the socket until the delimiter
  boost::system::error_code error;
  const auto bytesTransferred = read_until(
      socket, readBuffer, kDelimiter, error);
  if (error) {
    LOG(FATAL)
        << "Read error: "
        << boost::system::system_error(error).what();
  }

  // read_until may read more data into the buffer (past our delimiter)
  // so we need to extract based on the bytesTransferred value
  const string readString(
      buffers_begin(readBuffer.data()),
      buffers_begin(readBuffer.data()) +
      bytesTransferred - kDelimiter.length());
  // consume all of the data (including the delimiter) from our read buffer
  readBuffer.consume(bytesTransferred);
  return readString;
}




void createMIMEtable(string mimetype_fname){
  
  fstream mimetype_f;
  
  mimetype_f.open(mimetype_fname);
  
  //Check MIME type file name
  if(mimetype_f == 0){
    LOG(FATAL) << "MIME type file does not exist.";
  }else{

    string mimeline;
    //Read line until EOF 
    while(getline(mimetype_f, mimeline)){
      //Skip to next line if the line reads comment
      if(mimeline[0] == '#'){
        continue;
      }else{
        //parse line into words 
        
        string typestr, extstr, tst, ext; 
        stringstream ss(mimeline);

        if(ss){
          //Extract words to type/subtype and extension
          ss >> typestr >> extstr;

          //If line isn't blank
          if( typestr.length() > 0 && extstr.length() > 0){
          
            //Validate type/subtype and parse 
            if(typestr.find("type=") == 0 &&   
             count(typestr.begin(), typestr.end(), '=') == 1 && 
             count(typestr.begin(), typestr.end(), '/') ==1 ){

               tst = typestr.substr(5, typestr.length() - 5); 
            }
            else{ 
                 LOG(FATAL) << "MIME type file error: wrong format "; 
            }
            
            //Validate extension and parse
            if(extstr.find("exts=") == 0 &&
              count(extstr.begin(), extstr.end(), '=') == 1){
              
                int num = count(extstr.begin(), extstr.end(), ',');
                extstr = extstr.substr(5, extstr.length() - 5);

                //For each extension type in ext string
                for(int i = 0; i <= num; i++ ){
                
                  //parse ext by comma
                  if(i == num){
                     ext = extstr;
                  }
                  else{ 
                    int idx = extstr.find(",");
                    ext = extstr.substr(0, idx );
                    extstr.erase(0,idx + 1);
                  }

                   //Validate ext repetition and insert ext into table
                  if(typeLookup.find(ext) == typeLookup.end()){
                    typeLookup.insert(make_pair(ext, tst));
                  }
                  else{
                   LOG(FATAL) << "MIME type file error: extension cannot appear twice.";
                  }
                }
              }
              else{ LOG(FATAL) << "MIME type file error: wrong format "; }

          }
        }
      }
    }
  }

}

