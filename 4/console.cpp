#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/tokenizer.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace std;

boost::asio::io_service global_io_service;
typedef basic_string<unsigned char> ustring;

struct host_info {
	bool is_set = true;
	string addr;
	string port;
	string file;
};

struct host_info host[5];
struct host_info socks_server;

void get_host_info() {
    string query = getenv("QUERY_STRING");
    boost::char_separator<char> sep("&=", 0, boost::keep_empty_tokens);
    boost::tokenizer<boost::char_separator<char>> tokens(query, sep);
    boost::tokenizer<boost::char_separator<char>>::iterator tok_iter;

    tok_iter = tokens.begin();
    for (int i = 0; i < 5; ++i) {
        ++tok_iter;
        host[i].addr = *tok_iter;
        if(tok_iter->empty())
            host[i].is_set = false;

        ++tok_iter;
        ++tok_iter;
        host[i].port = *tok_iter;
        if(tok_iter->empty())
            host[i].is_set = false;

        ++tok_iter;
        ++tok_iter;
        host[i].file = *tok_iter;
        if(tok_iter->empty())
            host[i].is_set = false;

        ++tok_iter;
        cerr << host[i].addr << ":" << host[i].port << "<" << host[i].file << endl;
    }

    ++tok_iter;
    socks_server.addr = *tok_iter;
    if(tok_iter->empty())
        socks_server.is_set = false;

    ++tok_iter;
    ++tok_iter;
    socks_server.port = *tok_iter;
    if(tok_iter->empty())
        socks_server.is_set = false;

    cerr << socks_server.addr << ":" << socks_server.port << endl;
}

void output_content() {
    cout << "Content-type: text/html\r\n\r\n";
    cout << "<!DOCTYPE html>\n"
            "<html lang=\"en\">\n"
            "<head>\n"
            "    <meta charset=\"UTF-8\" />\n"
            "    <title>NP Project 3 Console</title>\n"
            "    <link "
            "    rel=\"stylesheet\""
            "    href=\"https://stackpath.bootstrapcdn.com/bootstrap/4.1.3/css/bootstrap.min.css\""
            "    integrity=\"sha384-MCw98/SFnGE8fJT3GXwEOngsV7Zt27NXFoaoApmYm81iuXoPkFOJwJ8ERdknLPMO\""
            "    crossorigin=\"anonymous\""
            "    />\n"
            "    <link"
            "    href=\"https://fonts.googleapis.com/css?family=Source+Code+Pro\""
            "    rel=\"stylesheet\""
            "    />\n"
            "    <link"
            "    rel=\"icon\""
            "    type=\"image/png\""
            "    href=\"https://cdn0.iconfinder.com/data/icons/small-n-flat/24/678068-terminal-512.png\""
            "    />\n"
            "    <style>\n"
            "    * {\n"
            "        font-family: 'Source Code Pro', monospace;\n"
            "        font-size: 1rem !important;\n"
            "    }\n"
            "    body {background-color: #212529;}\n"
            "    pre {color: #cccccc;}\n"
            "    b {color: #ffffff;}\n"
            "    </style>\n"
            "</head>\n"
            "<body>\n"
            "    <table class=\"table table-dark table-bordered\">\n"
            "    <thead>\n"
            "        <tr>\n";
    for (int i = 0; i < 5; i++) {
        if (host[i].is_set)
            cout << "        <th scope=\"col\">" << host[i].addr << ":" << host[i].port << "</th>\n";
    }
    cout << "        </tr>\n"
            "    </thead>\n"
            "    <tbody>\n"
            "        <tr>\n";
    for (int i = 0; i < 5; i++) {
        if (host[i].is_set)
            cout << "        <td><pre id=\"s" << i << "\" class=\"mb-0\"></pre></td>\n";
    }
    cout << "        </tr>\n"
            "    </tbody>\n"
            "    </table>\n"
            "</body>\n"
            "</html>\n";
}

class ShellSession : public enable_shared_from_this<ShellSession> {
 private:
  enum { max_length = 15000 };
  boost::asio::ip::tcp::socket _socket;
	boost::asio::ip::tcp::resolver _resolver;
	int _session_id;
	string _data;
	ifstream _read_file;

 public:
  ShellSession(int session_id)
      :	_socket(global_io_service),
        _resolver(global_io_service) {
      _data.resize(max_length);
      _session_id = session_id;
  }

  void start() { 
		do_resolve();
	}

 private:

  void do_resolve() {
    auto self(shared_from_this());
    boost::asio::ip::tcp::resolver::query query(host[_session_id].addr, host[_session_id].port);
    _resolver.async_resolve(
				query, 
				[this, self](boost::system::error_code ec, boost::asio::ip::tcp::resolver::iterator iter) {
          if (!ec) {
						boost::asio::ip::tcp::endpoint endpoint = *iter;
						connect_to_socks_server(endpoint);
					}
        }
		);
  }

  void connect_to_socks_server(boost::asio::ip::tcp::endpoint client_endpoint) {
    auto self(shared_from_this());
    boost::asio::ip::tcp::resolver::query query(socks_server.addr, socks_server.port);
    boost::asio::connect(_socket, _resolver.resolve(query)); 
    ustring msg(8, '\0');
    msg[0] = 4;
    msg[1] = 1;
    msg[2] = client_endpoint.port() / 256;
    msg[3] = client_endpoint.port() % 256;
    msg[4] = client_endpoint.address().to_v4().to_bytes()[0];
    msg[5] = client_endpoint.address().to_v4().to_bytes()[1];
    msg[6] = client_endpoint.address().to_v4().to_bytes()[2];
    msg[7] = client_endpoint.address().to_v4().to_bytes()[3];
    _socket.send(boost::asio::buffer(msg, 8));
    _socket.receive(boost::asio::buffer(msg, 8));
    if (msg[1] != 90)
      _socket.close();
    host[_session_id].file.insert(0, "test_case/");
    _read_file.open(host[_session_id].file);
    if (_read_file.is_open()) {
      do_read();
    }
  }

  void do_read() {
    auto self(shared_from_this());
    _socket.async_read_some(
        boost::asio::buffer(_data, max_length),
        [this, self](boost::system::error_code ec, size_t length) {
          if (!ec) {
						// cerr << length << endl;
						string command;
						string output(_data.begin(), _data.begin()+length);
						output_shell(output, length);
						if (output.find("% ") == string::npos) {
							do_read();
						}
						else {
							if(getline(_read_file, command)) {
								command += '\n';
								output_command(command, command.length());
								do_write(command, command.length());
							}
						}
					}
          else {
            // cerr << "Read Error: " << ec.message() << endl;
            _read_file.close();
            _socket.close();
          }
        }
		);
  }

  void do_write(string buf, size_t length) {
    auto self(shared_from_this());
    _socket.async_send(
        boost::asio::buffer(buf, length),
        [this, self](boost::system::error_code ec, size_t /* length */) {
          if (!ec) {
						do_read();
					}
        }
		);
  }

	void output_shell(string msg, size_t length) {
    auto self(shared_from_this());
    // cerr << msg;
    using boost::algorithm::replace_all;
    replace_all(msg, "&", "&amp;");
    replace_all(msg, "\"", "&quot;");
    replace_all(msg, "\'", "&apos;");
    replace_all(msg, "<", "&lt;");
    replace_all(msg, ">", "&gt;");
    replace_all(msg, "\n", "&#13;");
    replace_all(msg, "\r", "");
    cout << "<script>document.getElementById('s" << _session_id << "').innerHTML += '" << msg << "';</script>\r\n" << flush;
  }

  void output_command(string msg, size_t length) {
    auto self(shared_from_this());
    // cerr << msg;
    using boost::algorithm::replace_all;
    replace_all(msg, "&", "&amp;");
    replace_all(msg, "\"", "&quot;");
    replace_all(msg, "\'", "&apos;");
    replace_all(msg, "<", "&lt;");
    replace_all(msg, ">", "&gt;");
    replace_all(msg, "\n", "&#13;");
    replace_all(msg, "\r", "");
    cout << "<script>document.getElementById('s" << _session_id << "').innerHTML += '<b>" << msg << "</b>';</script>\r\n" << flush;
  }
};

int main(int argc, char* const argv[]) {
  try {
    get_host_info();
    output_content();

    for (int i = 0; i < 5; i++) {
      if (host[i].is_set) {
        make_shared<ShellSession>(i)->start();
      }
    }

    global_io_service.run();
  }
  catch (exception& e) {
    // cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}