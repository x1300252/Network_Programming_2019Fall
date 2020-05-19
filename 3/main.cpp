#include <boost/asio.hpp>
#include <boost/tokenizer.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <memory>
#include <utility>
#include <string>
#include <unistd.h>

using namespace std;

boost::asio::io_service global_io_service;

struct host_info {
    bool is_set = true;
    string addr;
    string port;
    string file;
} host[5];

void get_host_info(string query) {
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
        // cerr << host[i].addr << ":" << host[i].port << "<" << host[i].file << endl;
    }
}

class HttpSession : public enable_shared_from_this<HttpSession> {
 private:
  enum { max_length = 1024 };
  boost::asio::ip::tcp::socket _socket;
  string _data;

 public:
  HttpSession(boost::asio::ip::tcp::socket socket) : _socket(move(socket)) {
    _data.resize(max_length);
  }

  boost::asio::ip::tcp::socket& socket() {
    return _socket;
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
            cout << _data;
            string uri = get_uri(_data);

            if(uri.compare("/panel.cgi") == 0) {
              string content = get_panel_cgi();
              do_write(content, content.length());
              close_connection();
            }
            else if(uri.compare("/console.cgi") == 0) {
              string query = get_query(_data);
              // cerr << query << endl;
              get_host_info(query);
              string content = get_console_cgi();
              do_write(content, content.length());
              for (int i = 0; i < 5; i++) {
                if (host[i].is_set) {
                  make_shared<ShellSession>(shared_from_this(), i)->start();
                }
              }
            }
          }
        }
    );
  }

  void do_write(string buf, size_t length) {
    auto self(shared_from_this());
    _socket.async_send(
        boost::asio::buffer(buf, length),
        [this, self](boost::system::error_code ec, size_t /* length */) {
          if (!ec)
            return;
        }
    );
  }

  void close_connection() {
    _socket.shutdown(boost::asio::ip::tcp::socket::shutdown_send);
  }

  class ShellSession : public enable_shared_from_this<ShellSession> {
  private:
    enum { max_length = 15000 };
    boost::asio::ip::tcp::socket _socket;
    boost::asio::ip::tcp::resolver _resolver;
    int _session_id;
    string _data;
    ifstream _read_file;
    shared_ptr<HttpSession> _http_session;

  public:
    ShellSession(const shared_ptr<HttpSession>& http_session, int session_id)
        :	_socket(global_io_service),
          _resolver(global_io_service),
          _http_session(http_session) {
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
              do_connect(endpoint);
            }
          }
      );
    }

    void do_connect(boost::asio::ip::tcp::endpoint endpoint) {
      auto self(shared_from_this());
      _socket.async_connect(
          endpoint,
          [this, self](boost::system::error_code ec) {
            if (!ec) {
              host[_session_id].file.insert(0, "test_case/");
              _read_file.open(host[_session_id].file);
              if (_read_file.is_open()) {
                do_read();
              }
            }
          }
      );
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
                else {
                  _read_file.close();
                  close_connection();
                }
              }
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

    void close_connection() {
      auto self(shared_from_this());
      _socket.close();
    }

    void output_shell(string msg, size_t length) {
      auto self(shared_from_this());
      cerr << msg;
      boost::replace_all(msg, "&", "&amp;");
      boost::replace_all(msg, "\"", "&quot;");
      boost::replace_all(msg, "\'", "&apos;");
      boost::replace_all(msg, "<", "&lt;");
      boost::replace_all(msg, ">", "&gt;");
      boost::replace_all(msg, "\n", "&#13;");
      boost::replace_all(msg, "\r", "");
      string buf;
      buf += "<script>document.getElementById('s" + to_string(_session_id) + "').innerHTML += '" + msg + "';</script>\n";
      boost::asio::write(_http_session->socket(), boost::asio::buffer(buf, buf.length()));
    }

    void output_command(string msg, size_t length) {
      auto self(shared_from_this());
      cerr << msg;
      boost::replace_all(msg, "&", "&amp;");
      boost::replace_all(msg, "\"", "&quot;");
      boost::replace_all(msg, "\'", "&apos;");
      boost::replace_all(msg, "<", "&lt;");
      boost::replace_all(msg, ">", "&gt;");
      boost::replace_all(msg, "\n", "&#13;");
      boost::replace_all(msg, "\r", "");
      string buf;
      buf += "<script>document.getElementById('s" + to_string(_session_id) + "').innerHTML += '" + msg + "';</script>\n";
      boost::asio::write(_http_session->socket(), boost::asio::buffer(buf, buf.length()));
    }
  };

  string get_uri(string msg) {
    boost::char_separator<char> sep(" :\r\n", "?");
    boost::tokenizer< boost::char_separator<char> > tokens(msg, sep);
    boost::tokenizer< boost::char_separator<char> >::iterator tok_iter;
    string uri;

    tok_iter = tokens.begin();
    ++tok_iter;
    cout << "REQUEST_URI=" << *tok_iter << endl;
    uri = *tok_iter;

    return uri;
  }

  string get_query(string msg) {
    boost::char_separator<char> sep(" :\r\n", "?");
    boost::tokenizer< boost::char_separator<char> > tokens(msg, sep);
    boost::tokenizer< boost::char_separator<char> >::iterator tok_iter;
    string query;

    tok_iter = tokens.begin();
    ++tok_iter;
    ++tok_iter;
    if(*tok_iter == string("?")) {
      ++tok_iter;
      cout << "QUERY_STRING=" << *tok_iter << endl;
      query = *tok_iter;
    }
    else {
      cout << "QUERY_STRING=" << endl;
      query = "";
    }

    return query;
  }

  string get_panel_cgi() {
    string host_list;
    for (int i=1; i<13; i++) {
      host_list += "<option value=\"nplinux" + to_string(i) +".cs.nctu.edu.tw\">nplinux" + to_string(i) + "</option>\n";
    }

    string test_case_list;
    for (int i=1; i<11; i++) {
      test_case_list += "<option value=\"t" + to_string(i) + ".txt\">t" + to_string(i) + ".txt</option>\n";
    }
    string content;
    content += "HTTP/1.1 200 OK\r\n";
    content += "Content-type: text/html\r\n\r\n";
    content += "<!DOCTYPE html>\n"
              "<html lang=\"en\">\n"
              "<head>\n"
              "    <title>NP Project 3 Panel</title>\n"
              "    <link "
              "    rel=\"stylesheet\""
              "    href=\"https://stackpath.bootstrapcdn.com/bootstrap/4.1.3/css/bootstrap.min.css\""
              "    integrity=\"sha384-MCw98/SFnGE8fJT3GXwEOngsV7Zt27NXFoaoApmYm81iuXoPkFOJwJ8ERdknLPMO\""
              "    crossorigin=\"anonymous\""
              "    />\n"
              "    <link "
              "    href=\"https://fonts.googleapis.com/css?family=Source+Code+Pro\""
              "    rel=\"stylesheet\""
              "    />\n"
              "    <link "
              "    rel=\"icon\""
              "    type=\"image/png\""
              "    href=\"https://cdn4.iconfinder.com/data/icons/iconsimple-setting-time/512/dashboard-512.png\""
              "    />\n"
              "    <style>\n"
              "    * { font-family: 'Source Code Pro', monospace; }\n"
              "    </style>\n"
              "</head>\n"
              "<body class=\"bg-secondary pt-5\">\n";
    content += "<form action=\"console.cgi\" method=\"GET\">\n"
              "<table class=\"table mx-auto bg-light\" style=\"width: inherit\">\n"
              "  <thead class=\"thead-dark\">\n"
              "     <tr>\n"
              "       <th scope=\"col\">#</th>\n"
              "       <th scope=\"col\">Host</th>\n"
              "       <th scope=\"col\">Port</th>\n"
              "       <th scope=\"col\">Input File</th>\n"
              "     </tr>\n"
              "  </thead>\n"
              "  <tbody>\n";
    for (int i=0; i<5; i++) {
      content += "<tr>\n"
                "<th scope=\"row\" class=\"align-middle\">Session " + to_string(i+1) + "</th>\n"
                "<td>\n"
                "  <div class=\"input-group\">\n"
                "    <select name=\"h" + to_string(i) + "\" class=\"custom-select\">\n"
                "      <option></option>\n" + host_list;
      content += "    </select>\n"
                "    <div class=\"input-group-append\">\n"
                "      <span class=\"input-group-text\">.cs.nctu.edu.tw</span>\n"
                "    </div>\n"
                "  </div>\n"
                "</td>\n"
                "<td>\n"
                "  <input name=\"p" + to_string(i) + "\" type=\"text\" class=\"form-control\" size=\"5\" />\n"
                "</td>\n"
                "<td>\n"
                "  <select name=\"f" + to_string(i) + "\" class=\"custom-select\">\n"
                "    <option></option>\n";
      content += test_case_list;
      content += "  </select>\n"
                "</td\n>"
                "</tr>\n";
    }
    content += "     <tr>\n"
              "       <td colspan=\"3\"></td>\n"
              "       <td>\n"
              "         <button type=\"submit\" class=\"btn btn-info btn-block\">Run</button>\n"
              "       </td>\n"
              "    </tr>\n"
              "  </tbody>\n"
              "  </table>\n"
              "  </form>\n"
              "</body>\n"
              "</html>\n";
    
    return content;
  }

  string get_console_cgi() {
    string content;
    content += "HTTP/1.1 200 OK\r\n";
    content += "Content-type: text/html\r\n\r\n";
    content += "<!DOCTYPE html>\n"
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
            content += "        <th scope=\"col\">" + host[i].addr + ":" + host[i].port + "</th>\n";
    }
    content += "        </tr>\n"
              "    </thead>\n"
              "    <tbody>\n"
              "        <tr>\n";
    for (int i = 0; i < 5; i++) {
        if (host[i].is_set)
            content += "        <td><pre id=\"s" + to_string(i) + "\" class=\"mb-0\"></pre></td>\n";
    }
    content += "        </tr>\n"
              "    </tbody>\n"
              "    </table>\n"
              "</body>\n"
              "</html>\n";
    return content;
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
        make_shared<HttpSession>(move(_socket))->start();   
      }

      do_accept();
    });
  }
};

int main(int argc, char* const argv[]) {
  if (argc != 2) {
    cerr << "Usage:" << argv[0] << " [port]" << endl;
    return 1;
  }

  try {
    unsigned short port = atoi(argv[1]);
    HttpServer server(port);
    global_io_service.run();
  } catch (exception& e) {
    cerr << "Exception: " << e.what() << "\n";
  }

  return 0;
}