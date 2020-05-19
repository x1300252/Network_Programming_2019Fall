#include <boost/asio.hpp>
#include <boost/tokenizer.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <string>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;

boost::asio::io_service global_io_service;

class HttpSession : public enable_shared_from_this<HttpSession> {
 private:
  enum { max_length = 1024 };
  boost::asio::ip::tcp::socket _socket;
  string _data;

 public:
  HttpSession(boost::asio::ip::tcp::socket socket) : _socket(move(socket)) {
    _data.resize(max_length);
  }

  void start() { 
    do_read();
  }

 private:
  void do_read() {
    auto self(shared_from_this());
    _socket.async_read_some(
        boost::asio::buffer(_data, max_length),
        [this, self](boost::system::error_code ec, size_t length) {
          if (!ec) {
            cout << endl; // << _data << endl;
            string uri = set_env_var(_data);
            char* path = strdup(uri.insert(0, ".").c_str());
            char* fname = strdup(uri.erase(0, 2).c_str());
            
            dup2(_socket.native_handle(), STDOUT_FILENO);
            // dup2(_socket.native_handle(), STDERR_FILENO);
            cout << "HTTP/1.1 200 OK\r\n";
            execl(path, fname);
          }
        }
    );
  }

  void do_write(size_t length) {
    auto self(shared_from_this());
    _socket.async_send(
        boost::asio::buffer(_data, length),
        [this, self](boost::system::error_code ec, size_t /* length */) {
          if (!ec)
            return;
        }
    );
  }

  string set_env_var(string msg) {
    boost::char_separator<char> sep(" :\r\n", "?");
    boost::tokenizer< boost::char_separator<char> > tokens(msg, sep);
    boost::tokenizer< boost::char_separator<char> >::iterator tok_iter;
    string addr, uri;

    tok_iter = tokens.begin();
    // cout << "REQUEST_METHOD=" << *tok_iter << endl;
    setenv("REQUEST_METHOD", tok_iter->c_str(), 1);

    ++tok_iter;
    // cout << "REQUEST_URI=" << *tok_iter << endl;
    setenv("REQUEST_URI", tok_iter->c_str(), 1);
    uri = *tok_iter;

    ++tok_iter;
    if(*tok_iter == string("?")) {
      ++tok_iter;
      // cout << "QUERY_STRING=" << *tok_iter << endl;
      setenv("QUERY_STRING", tok_iter->c_str(), 1);
      ++tok_iter;
    }
    else {
      // cout << "QUERY_STRING=" << endl;
      setenv("QUERY_STRING", "", 1);
    }

    // cout << "SERVER_PROTOCOL=" << *tok_iter << endl;
    setenv("SERVER_PROTOCOL", tok_iter->c_str(), 1);

    ++tok_iter;
    ++tok_iter;
    // cout << "HTTP_HOST=" << *tok_iter << endl;
    setenv("HTTP_HOST", tok_iter->c_str(), 1);
    
    addr = *tok_iter;
    ++tok_iter;
    boost::asio::ip::tcp::resolver resolver(global_io_service);
    boost::asio::ip::tcp::resolver::query query(addr, *tok_iter);
    boost::asio::ip::tcp::resolver::iterator iter = resolver.resolve(query);
    boost::asio::ip::tcp::resolver::iterator end;
    if (iter != end) {
      boost::asio::ip::tcp::endpoint endpoint = *iter;
      // cout << "SERVER_ADDR=" << endpoint.address().to_string() << endl;  
      setenv("SERVER_ADDR", endpoint.address().to_string().c_str(), 1);
    }

    // cout << "SERVER_PORT=" << *tok_iter << endl;
    setenv("SERVER_PORT", tok_iter->c_str(), 1);

    // cout << "REMOTE_ADDR=" << _socket.remote_endpoint().address().to_string() << endl;
    setenv("REMOTE_ADDR", _socket.remote_endpoint().address().to_string().c_str(), 1);

    // cout << "REMOTE_PORT=" << _socket.remote_endpoint().port() << endl;
    setenv("REMOTE_PORT", to_string(_socket.remote_endpoint().port()).c_str(), 1);
    
    return uri;
  }
};

class HttpServer {
 private:
  boost::asio::ip::tcp::acceptor _acceptor;
  boost::asio::ip::tcp::socket _socket;

 public:
  HttpServer(short port)
      : _acceptor(global_io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
        _socket(global_io_service) {
    do_accept();
  }

 private:
  void do_accept() {
    _acceptor.async_accept(_socket, [this](boost::system::error_code ec) {
      if (!ec) {
        global_io_service.notify_fork(boost::asio::io_context::fork_prepare);

        if (fork() == 0) {
          global_io_service.notify_fork(boost::asio::io_context::fork_child);
          make_shared<HttpSession>(move(_socket))->start();
          _acceptor.close();
        }
        else {
          global_io_service.notify_fork(boost::asio::io_context::fork_parent);
          _socket.close();

          do_accept();
        }
      }
    });
  }
};

int main(int argc, char* const argv[]) {
  if (argc != 2) {
    cerr << "Usage:" << argv[0] << " [port]" << endl;
    return 1;
  }

  try {
    signal(SIGCHLD, SIG_IGN);
    unsigned short port = atoi(argv[1]);
    HttpServer server(port);
    global_io_service.run();
  } catch (exception& e) {
    cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}