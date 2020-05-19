#include <boost/asio.hpp>
#include <boost/tokenizer.hpp>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <memory>
#include <utility>
#include <string>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace std;

typedef struct {
  uint8_t VN;
  uint8_t CD;
  uint8_t DSPORT[2];
  uint8_t DSIP[4];
  char *user_id;
  char *domain_name;
} Socks4Request;

typedef basic_string<unsigned char> ustring;

boost::asio::io_service global_io_service;

class Socks4Session : public enable_shared_from_this<Socks4Session> {
 private:
  enum { max_length = 1024 };
  boost::asio::ip::tcp::socket _socket;
  boost::asio::ip::tcp::socket _dst_socket;
  boost::asio::ip::tcp::resolver _resolver;
  ustring _data;
  ustring _buffer1;
  ustring _buffer2;

 public:
  Socks4Session(boost::asio::ip::tcp::socket socket) : 
    _socket(move(socket)), 
    _dst_socket(global_io_service),
    _resolver(global_io_service) {
    _data.resize(max_length);
    _buffer1.resize(max_length);
    _buffer2.resize(max_length);
  }

  void start() { 
    recv_socks4_req();
  }

 private:
  void recv_socks4_req() {
    auto self(shared_from_this());
    _socket.async_read_some(
        boost::asio::buffer(_data, max_length),
        [this, self](boost::system::error_code ec, size_t length) {
          if (!ec) {
            if (_data[0] == 4) {
              cout << "<S_IP>: " << _socket.remote_endpoint().address().to_string() << endl;
              cout << "<S_PORT>: " << _socket.remote_endpoint().port() << endl;

              string ip = to_string(int(_data[4])) + "." + to_string(int(_data[5])) + "." + to_string(int(_data[6])) + "." + to_string(int(_data[7]));
              string port = to_string((unsigned short)(_data[2]) * 256 + (unsigned short)(_data[3]));

              if (ip.find("0.0.0.") != string::npos) {
                size_t pos = _data.find_first_of('\0', 8);
                string domain_name((const char*)_data.substr(pos+1).c_str());
                boost::asio::ip::tcp::resolver::query query(domain_name, port);
                boost::asio::ip::tcp::resolver::iterator iter = _resolver.resolve(query);
                boost::asio::ip::tcp::endpoint tmp = *iter;
                ip = tmp.address().to_v4().to_string();
              }

              cout << "<D_IP>: " << ip << endl;
              cout << "<D_PORT>: " << port << endl;

              if (_data[1] == 1)  {
                cout << "<Command>: CONNECT" << endl;
                if (!check_firewall(ip, _data[1])) {
                  cout << "<Reply>: Reject" << endl;
                  send_socks4_reply(false, 0);
                  return;
                }
                do_resolve(ip, port);
              }
              else if (_data[1] == 2) {
                cout << "<Command>: BIND" << endl;
                if (!check_firewall(ip, _data[1])) {
                  cout << "<Reply>: Reject" << endl;
                  send_socks4_reply(false, 0);
                  return;
                }
                do_accept();
              }
            }
          }
        }
    );
  }

  bool check_firewall(string dst_ip, int mode) {
    ifstream firewall_file;
    firewall_file.open("./socks.conf");
    string rule;
    string prefix = "permit " + (mode == 1) ? "c " : "b ";
    size_t pos;
    string permit_prefix;
    if (firewall_file.is_open()) {
      while (getline(firewall_file, rule)) {
        if (rule.find(prefix) != string::npos) {
          pos = rule.find_first_of("*");
          permit_prefix = rule.substr(9, pos);
          if (rule.compare(9, pos-9, dst_ip, 0, pos-9) == 0)
            return true;
        }
      }
      firewall_file.close();
      return false;
    }
    return true;
  }

  void do_resolve(string ip, string port) {
    auto self(shared_from_this());
    boost::asio::ip::tcp::resolver::query query(ip, port);
    _resolver.async_resolve(
				query, 
				[this, self](boost::system::error_code ec, boost::asio::ip::tcp::resolver::iterator iter) {
          if (!ec) {
						boost::asio::ip::tcp::endpoint endpoint = *iter;
						connect_to_dst(endpoint);
					}
        }
		);
  }

  void connect_to_dst(boost::asio::ip::tcp::endpoint endpoint) {
    auto self(shared_from_this());
    _dst_socket.async_connect(
				endpoint,
				[this, self](boost::system::error_code ec) {
          if (!ec) {
            send_socks4_reply(true, 0);
            cout << "<Reply>: Accept" << endl;
            read_from_browser();
            read_from_dst();
            return;
					}
          else {
            send_socks4_reply(false, 0);
            cout << "<Reply>: Reject" << endl;
            cerr << ec.category().name() << ": " << ec.message() << endl;
          }
        }
		);
  }

  void do_accept() {
    boost::asio::ip::tcp::acceptor _acceptor(global_io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));
    int port = _acceptor.local_endpoint().port();
    send_socks4_reply(true, port);
    _acceptor.accept(_dst_socket);
    send_socks4_reply(true, port);
    cout << "<Reply>: Accept" << endl;
    read_from_browser();
    read_from_dst();
  }

  void send_socks4_reply(bool accept, int port) {
    auto self(shared_from_this());
    ustring reply_msg(8, '\0');
    reply_msg[1] = accept ? 90 : 91;
    reply_msg[2] = port / 256;
    reply_msg[3] = port % 256;
    _socket.send(boost::asio::buffer(reply_msg, 8));
  }

  void read_from_browser() {
    auto self(shared_from_this());
    _socket.async_read_some(
        boost::asio::buffer(_buffer1, max_length),
        [this, self](boost::system::error_code ec, size_t length) {
          if (!ec) {
						write_to_dst(_buffer1, length);
          }
          else {
            _dst_socket.close();
            _socket.close();
            cerr << ec.category().name() << ": " << ec.message() << endl;
          }
        }
		);
  }

  void write_to_browser(ustring buf, size_t buf_length) {
    auto self(shared_from_this());
    async_write(
        _socket,
        boost::asio::buffer(buf, buf_length),
        [this, self](boost::system::error_code ec, size_t length) {
          if (!ec) {
						read_from_dst();
					}
          else {
            _dst_socket.close();
            _socket.close();
            cerr << ec.category().name() << ": " << ec.message() << endl;
          }
        }
		);
  }

  void read_from_dst() {
    auto self(shared_from_this());
    _dst_socket.async_read_some(
        boost::asio::buffer(_buffer2, max_length),
        [this, self](boost::system::error_code ec, size_t length) {
          if (!ec) {
						write_to_browser(_buffer2, length);
          }
          else {
            _dst_socket.close();
            _socket.close();
            cerr << ec.category().name() << ": " << ec.message() << endl;
          }
        }
		);
  }
  
  void write_to_dst(ustring buf, size_t length) {
    auto self(shared_from_this());
    async_write(
        _dst_socket,
        boost::asio::buffer(buf, length),
        [this, self](boost::system::error_code ec, size_t /* length */) {
          if (!ec) {
						read_from_browser();
					}
          else {
            _dst_socket.close();
            _socket.close();
            cerr << ec.category().name() << ": " << ec.message() << endl;
          }
        }
		);
  }
};

class Socks4Server {
 private:
  boost::asio::ip::tcp::acceptor _acceptor;
  boost::asio::ip::tcp::socket _socket;

 public:
  Socks4Server(short port)
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
          _acceptor.close();
          make_shared<Socks4Session>(move(_socket))->start();
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
    // signal(SIGCHLD, SIG_IGN);
    unsigned short port = atoi(argv[1]);
    Socks4Server server(port);
    global_io_service.run();
  } catch (exception& e) {
    cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}