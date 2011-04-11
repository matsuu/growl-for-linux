/* Copyright 2011 by Yasuhiro Matsumoto
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <gtk/gtk.h>
#ifdef _WIN32
# include <gdk/gdkwin32.h>
# include <ws2tcpip.h>
#else
# include <netinet/tcp.h>
# include <sys/socket.h>
# include <netdb.h>
# include <unistd.h>
#endif
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <curl/curl.h>
#ifdef _WIN32
# include <io.h>
#endif
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/des.h>

#ifdef _WIN32
typedef char sockopt_t;
typedef int socklen_t;
# ifndef snprintf
#  define snprintf _snprintf
# endif
# ifndef strncasecmp
#  define strncasecmp(d,s,n) strnicmp(d,s,n)
# endif
# ifndef srandom
#  define srandom(s) srand(s)
# endif
# ifndef random
#  define random() rand()
# endif
#else
# define closesocket(x) close(x)
typedef int sockopt_t;
#endif

#define REQUEST_TIMEOUT            (5)

static GList* notifications = NULL;
static gchar* password = "123456"; // should be configuable.
static gboolean main_loop = TRUE;

typedef struct {
  int sock;

  gint pos;
  gint x, y;
  gchar* title;
  gchar* text;
  gchar* icon;
  gchar* url;
  gint timeout;

  GtkWidget* popup;
  gint offset;

} NOTIFICATION_INFO;

typedef struct {
  char* data;     // response data from server
  size_t size;    // response size of data
} MEMFILE;

static MEMFILE*
memfopen() {
  MEMFILE* mf = (MEMFILE*) malloc(sizeof(MEMFILE));
  if (mf) {
    mf->data = NULL;
    mf->size = 0;
  }
  return mf;
}

static void
memfclose(MEMFILE* mf) {
  if (mf->data) free(mf->data);
  free(mf);
}

static size_t
memfwrite(char* ptr, size_t size, size_t nmemb, void* stream) {
  MEMFILE* mf = (MEMFILE*) stream;
  int block = size * nmemb;
  if (!mf) return block; // through
  if (!mf->data)
    mf->data = (char*) malloc(block);
  else
    mf->data = (char*) realloc(mf->data, mf->size + block);
  if (mf->data) {
    memcpy(mf->data + mf->size, ptr, block);
    mf->size += block;
  }
  return block;
}

static char*
memfstrdup(MEMFILE* mf) {
  char* buf;
  if (mf->size == 0) return NULL;
  buf = (char*) malloc(mf->size + 1);
  memcpy(buf, mf->data, mf->size);
  buf[mf->size] = 0;
  return buf;
}

static char*
get_http_header_alloc(const char* ptr, const char* key) {
  const char* tmp = ptr;

  while (*ptr) {
    tmp = strpbrk(ptr, "\r\n");
    if (!tmp) break;
    if (!strncasecmp(ptr, key, strlen(key)) && *(ptr + strlen(key)) == ':') {
      size_t len;
      char* val;
      const char* top = ptr + strlen(key) + 1;
      while (*top && isspace(*top)) top++;
      if (!*top) return NULL;
      len = tmp - top + 1;
      val = malloc(len);
      memset(val, 0, len);
      strncpy(val, top, len-1);
      return val;
    }
    ptr = tmp + 1;
  }
  return NULL;
}

static GdkPixbuf*
url2pixbuf(const char* url, GError** error) {
  GdkPixbuf* pixbuf = NULL;
  GdkPixbufLoader* loader = NULL;
  GError* _error = NULL;

  if (!strncmp(url, "file:///", 8) || g_file_test(url, G_FILE_TEST_EXISTS)) {
    gchar* newurl = g_filename_from_uri(url, NULL, NULL);
    pixbuf = gdk_pixbuf_new_from_file(newurl ? newurl : url, &_error);
  } else {
    CURL* curl = NULL;
    MEMFILE* mbody;
    MEMFILE* mhead;
    char* head;
    char* body;
    unsigned long size;
    CURLcode res = CURLE_FAILED_INIT;

    curl = curl_easy_init();
    if (!curl) return NULL;

    mbody = memfopen();
    mhead = memfopen();

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, REQUEST_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, memfwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, mbody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, memfwrite);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, mhead);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    head = memfstrdup(mhead);
    memfclose(mhead);
    body = memfstrdup(mbody);
    size = mbody->size;
    memfclose(mbody);

    if (res == CURLE_OK) {
      char* ctype;
      char* csize;
      ctype = get_http_header_alloc(head, "Content-Type");
      csize = get_http_header_alloc(head, "Content-Length");

#ifdef _WIN32
      if (ctype &&
          (!strcmp(ctype, "image/jpeg") || !strcmp(ctype, "image/gif"))) {
        char temp_path[MAX_PATH];
        char temp_filename[MAX_PATH];
        FILE* fp;
        GetTempPath(sizeof(temp_path), temp_path);
        GetTempFileName(temp_path, "growl-for-linux-", 0, temp_filename);
        fp = fopen(temp_filename, "wb");
        if (fp) {
          fwrite(body, size, 1, fp);
          fclose(fp);
        }
        pixbuf = gdk_pixbuf_new_from_file(temp_filename, NULL);
        DeleteFile(temp_filename);
      } else
#endif
      {
        if (ctype)
          loader =
            (GdkPixbufLoader*) gdk_pixbuf_loader_new_with_mime_type(ctype,
                error);
        if (csize)
          size = atol(csize);
        if (!loader) loader = gdk_pixbuf_loader_new();
        if (body && gdk_pixbuf_loader_write(loader, (const guchar*) body,
              size, &_error)) {
          pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
        }
      }
      if (ctype) free(ctype);
      if (csize) free(csize);
      if (loader) gdk_pixbuf_loader_close(loader, NULL);
    } else {
      _error = g_error_new_literal(G_FILE_ERROR, res,
          curl_easy_strerror(res));
    }

    free(head);
    free(body);
  }

  /* cleanup callback data */
  if (error && _error) *error = _error;
  return pixbuf;
}

static gboolean
open_url(const gchar* url) {
#if defined(_WIN32)
  return (int) ShellExecute(NULL, "open", url, NULL, NULL, SW_SHOW) > 32;
#elif defined(MACOSX)
  GError* error = NULL;
  const gchar *argv[] = {"open", (gchar*) url, NULL};
 	return g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
#else
  GError* error = NULL;
  gchar *argv[] = {"xdg-open", (gchar*) url, NULL};
 	return g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
#endif
}

static void
notification_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  NOTIFICATION_INFO* ni = (NOTIFICATION_INFO*) user_data;
  if (ni->timeout >= 30) ni->timeout = 30;
  if (ni->url && *ni->url) open_url(ni->url);
}

static gboolean
notification_animation_func(gpointer data) {
  NOTIFICATION_INFO* ni = (NOTIFICATION_INFO*) data;

  if (ni->timeout-- < 0) {
    gtk_widget_destroy(ni->popup);
    notifications = g_list_remove(notifications, ni);
    g_free(ni->title);
    g_free(ni->text);
    g_free(ni->icon);
    g_free(ni->url);
    g_free(ni);
    return FALSE;
  }

  if (ni->offset < 160) {
    ni->offset += 2;
    gtk_window_resize(GTK_WINDOW(ni->popup), 180, ni->offset);
    gtk_window_move(GTK_WINDOW(ni->popup), ni->x, ni->y - ni->offset);
  }

  if (ni->timeout < 30) {
    gtk_window_set_opacity(GTK_WINDOW(ni->popup), (double) ni->timeout/30.0*0.8);
  }
  return TRUE;
}

static gint
notifications_compare(gconstpointer a, gconstpointer b) {
  return ((NOTIFICATION_INFO*)b)->pos < ((NOTIFICATION_INFO*)a)->pos;
}

static gint
notification_show(gpointer data) {
  NOTIFICATION_INFO* ni = (NOTIFICATION_INFO*) data;

  GdkColor color;
  gdk_color_parse ("white", &color);
  GtkWidget* fixed;
  GtkWidget* vbox;
  GtkWidget* hbox;
  GtkWidget* label;
  GtkWidget* image;
  GdkPixbuf* pixbuf;
  GdkScreen* screen;
  gint n, pos, len;
  gint x, y;
  gint monitor_num;
  GdkRectangle rect;

  len = g_list_length(notifications);
  for (pos = 0; pos < len; pos++) {
    NOTIFICATION_INFO* p = g_list_nth_data(notifications, pos);
    if (pos != p->pos) break;
  }

  screen = gdk_screen_get_default();
  monitor_num = gdk_screen_get_primary_monitor(screen);
  gdk_screen_get_monitor_geometry(screen, monitor_num, &rect);

  x = rect.x + rect.width - 180;
  y = rect.y + rect.height - 180;
  for (n = 0; n < pos; n++) {
    y -= 180;
    if (y < 0) {
      x -= 200;
      if (x < 0) {
        return FALSE;
      }
      y = rect.y + rect.height - 180;
    }
  }

  ni->pos = pos;
  notifications = g_list_insert_sorted(notifications, ni, notifications_compare);
  ni->x = x;
  ni->y = y + 200;

  ni->popup = gtk_window_new(GTK_WINDOW_POPUP);
  gtk_window_set_title(GTK_WINDOW(ni->popup), "growl-for-linux");
  gtk_window_set_resizable(GTK_WINDOW(ni->popup), TRUE);
  gtk_window_set_decorated(GTK_WINDOW(ni->popup), FALSE);
  gtk_window_set_keep_above(GTK_WINDOW(ni->popup), TRUE);

  gtk_window_stick(GTK_WINDOW(ni->popup));
  gtk_window_set_opacity(GTK_WINDOW(ni->popup), 0.8);
  gtk_widget_modify_bg(ni->popup, GTK_STATE_NORMAL, &color);

  fixed = gtk_fixed_new();
  gtk_container_set_border_width(GTK_CONTAINER(fixed), 10);
  gtk_container_add(GTK_CONTAINER(ni->popup), fixed);

  vbox = gtk_vbox_new(FALSE, 5);
  gtk_container_add(GTK_CONTAINER(fixed), vbox);

  hbox = gtk_hbox_new(FALSE, 5);
  pixbuf = url2pixbuf(ni->icon, NULL);
  if (pixbuf) {
    GdkPixbuf* tmp = gdk_pixbuf_scale_simple(pixbuf, 32, 32, GDK_INTERP_TILES);
    if (tmp) pixbuf = tmp;
  }
  image = gtk_image_new_from_pixbuf(pixbuf);
  gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, FALSE, 0);

  label = gtk_label_new(ni->title);
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

  label = gtk_label_new(ni->text);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
  gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, FALSE, 0);

  gtk_widget_set_size_request(ni->popup, 180, 1);

  gtk_widget_set_events(ni->popup, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(ni->popup), "button-press-event", G_CALLBACK(notification_clicked), ni);

  ni->offset = 0;
  ni->timeout = 500;

  gtk_window_move(GTK_WINDOW(ni->popup), ni->x, ni->y);
  gtk_widget_show_all(ni->popup);

#ifdef _WIN32
  SetWindowPos(GDK_WINDOW_HWND(ni->popup->window), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
#endif
  
  g_timeout_add(10, notification_animation_func, ni);

  return FALSE;
}

static long
readall(int fd, char** ptr) {
  int i = 0, r;
  *ptr = (char*) calloc(BUFSIZ, 1);
  while ((r = recv(fd, *ptr + i, BUFSIZ, 0)) > 0) {
    i += r;
    if (r > 2 && !strncmp(*ptr + i - 4, "\r\n\r\n", 4)) break;
    *ptr = realloc(*ptr, BUFSIZ + i);
  }
  return i;
}

unsigned int
unhex(unsigned char c) {
  if('0' <= c && c <= '9') return (c - '0');
  if('a' <= c && c <= 'f') return (0x0a + c - 'a');
  if('A' <= c && c <= 'F') return (0x0a + c - 'A');
  return 0;
}

static void
status_icon_popup(GtkStatusIcon* status_icon, guint button, guint32 activate_time, gpointer menu) {
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, gtk_status_icon_position_menu, status_icon, button, activate_time);
}

static void
settings_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  main_loop = FALSE;
}

static void
exit_clicked(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  main_loop = FALSE;
}

static gpointer
recv_thread(gpointer data) {
  int sock = (int) data;
  int need_to_show = 0;

  NOTIFICATION_INFO* ni = g_new0(NOTIFICATION_INFO, 1);
  if (!ni) {
    perror("g_new0");
  }
  ni->sock = sock;

  char* ptr;
  int r = readall(ni->sock, &ptr);
  char* top = ptr;
  if (!strncmp(ptr, "GNTP/1.0 ", 9)) {
    ptr += 9;
    if (!strncmp(ptr, "REGISTER ", 9)) {
      ptr += 9;
      // TODO: register
    } else
    if (!strncmp(ptr, "NOTIFY ", 7)) {
      ptr += 7;
      char* data = NULL;
      if (!strncmp(ptr, "NONE ", 5)) {
        ptr = strchr(ptr, '\r');
        if (!ptr) goto leave;
        *ptr++ = 0;
        *ptr++ = 0;
        data = (char*) calloc(r-(ptr-top)-4+1, 1);
        if (!data) goto leave;
        memcpy(data, ptr, r-(ptr-top)-4);
      } else {
        if (strncmp(ptr, "AES:", 4) &&
            strncmp(ptr, "DES:", 4) &&
            strncmp(ptr, "3DES:", 5)) goto leave;

        char* crypt_algorythm = ptr;
        ptr = strchr(ptr, ':');
        if (!ptr) goto leave;
        *ptr++ = 0;
        char* iv;
        iv = ptr;
        ptr = strchr(ptr, ' ');
        if (!ptr) goto leave;
        *ptr++ = 0;

        if (strncmp(ptr, "MD5:", 4) &&
            strncmp(ptr, "SHA1:", 5) &&
            strncmp(ptr, "SHA256:", 7)) goto leave;

        char* hash_algorythm = ptr;
        ptr = strchr(ptr, ':');
        if (!ptr) goto leave;
        *ptr++ = 0;
        char* key = ptr;
        ptr = strchr(ptr, '.');
        if (!ptr) goto leave;
        *ptr++ = 0;
        char* salt = ptr;

        ptr = strchr(ptr, '\r');
        if (!ptr) goto leave;
        *ptr++ = 0;
        *ptr++ = 0;

        int n, keylen, saltlen, ivlen;

        char hex[3];
        hex[2] = 0;
        saltlen = strlen(salt) / 2;
        for (n = 0; n < saltlen; n++)
          salt[n] = unhex(salt[n * 2]) * 16 + unhex(salt[n * 2 + 1]);
        keylen = strlen(key) / 2;
        for (n = 0; n < keylen; n++)
          key[n] = unhex(key[n * 2]) * 16 + unhex(key[n * 2 + 1]);
        ivlen = strlen(iv) / 2;
        for (n = 0; n < ivlen; n++)
          iv[n] = unhex(iv[n * 2]) * 16 + unhex(iv[n * 2 + 1]);

        char digest[32] = {0};
        memset(digest, 0, sizeof(digest));

        if (!strcmp(hash_algorythm, "MD5")) {
          MD5_CTX ctx;
          MD5_Init(&ctx);
          MD5_Update(&ctx, password, strlen(password));
          MD5_Update(&ctx, salt, saltlen);
          MD5_Final((unsigned char*) digest, &ctx);
        }
        if (!strcmp(hash_algorythm, "SHA1")) {
          SHA_CTX ctx;
          SHA1_Init(&ctx);
          SHA1_Update(&ctx, password, strlen(password));
          SHA1_Update(&ctx, salt, saltlen);
          SHA1_Final((unsigned char*) digest, &ctx);
        }
        if (!strcmp(hash_algorythm, "SHA256")) {
          SHA256_CTX ctx;
          SHA256_Init(&ctx);
          SHA256_Update(&ctx, password, strlen(password));
          SHA256_Update(&ctx, salt, saltlen);
          SHA256_Final((unsigned char*) digest, &ctx);
        }

        data = (char*) calloc(r, 1);
        if (!data) goto leave;
        if (!strcmp(crypt_algorythm, "AES")) {
          AES_KEY aeskey;
          AES_set_decrypt_key((unsigned char*) digest, 24 * 8, &aeskey);
          AES_cbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
              r-(ptr-top)-6, &aeskey, (unsigned char*) iv, AES_DECRYPT);
        }
        if (!strcmp(crypt_algorythm, "DES")) {
          des_key_schedule schedule;
          DES_set_key_unchecked((const_DES_cblock*) &digest, &schedule);
          DES_ncbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
              r-(ptr-top)-6, &schedule, (const_DES_cblock*) &iv, DES_DECRYPT);
        }
        if (!strcmp(crypt_algorythm, "3DES")) {
          char key1[8], key2[8], key3[8];
          memcpy(key1, digest+ 0, 8);
          memcpy(key2, digest+ 8, 8);
          memcpy(key3, digest+16, 8);
          des_key_schedule schedule1, schedule2, schedule3;
          DES_set_key_unchecked((const_DES_cblock*) &key1, &schedule1);
          DES_set_key_unchecked((const_DES_cblock*) &key2, &schedule2);
          DES_set_key_unchecked((const_DES_cblock*) &key3, &schedule3);
          des_ede3_cbc_encrypt((unsigned char*) ptr, (unsigned char*) data,
              r-(ptr-top)-6, schedule1, schedule2, schedule3,
              (const_DES_cblock*) &iv, DES_DECRYPT);
        }
      }

      ptr = data;
      while (*ptr) {
        char* line = ptr;
        ptr = strchr(ptr, '\r');
        if (!ptr) break;
        *ptr++ = 0;
        *ptr++ = 0;
        if (!strncmp(line, "Notification-Title:", 19)) {
          line += 20;
          while(isspace(*line)) line++;
          ni->title = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Text:", 18)) {
          line += 19;
          while(isspace(*line)) line++;
          ni->text = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Icon:", 18)) {
          line += 19;
          while(isspace(*line)) line++;
          ni->icon = g_strdup(line);
        }
        if (!strncmp(line, "Notification-Callback-Target:", 29)) {
          line += 30;
          while(isspace(*line)) line++;
          ni->url = g_strdup(line);
        }
      }
      need_to_show = 1;
      free(data);
    }
    ptr = "GNTP/1.0 OK\r\n\r\n";
    send(ni->sock, ptr, strlen(ptr), 0);
  } else {
    ptr = "GNTP/1.0 -ERROR Invalid command\r\n\r\n";
    send(ni->sock, ptr, strlen(ptr), 0);
  }
  free(top);
  closesocket(ni->sock);
  if (need_to_show)
    g_idle_add(notification_show, ni); // call once
  return NULL;

leave:
  free(top);
  closesocket(ni->sock);
  free(ni);
  return NULL;
}

int
main(int argc, char* argv[]) {
  int fd;
  struct sockaddr_in server_addr;
  fd_set fdset;
  struct timeval tv;
  GtkStatusIcon* status_icon;
  GtkWidget* menu;
  GtkWidget* menu_settings;
  GtkWidget* menu_exit;
  sockopt_t sockopt;

  if (argc != 2) {
    fprintf(stderr, "usage: gol [password]\n");
    exit(1);
  }

  // TODO: setttign dialog
  password = argv[1];

  tv.tv_sec = 0;
  tv.tv_usec = 0;

#ifdef _WIN32
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    exit(1);
  }

  sockopt = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt)) == -1) {
    perror("setsockopt");
    exit(1);
  }
  sockopt = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &sockopt, sizeof(sockopt)) == -1) {
    perror("setsockopt");
    exit(1);
  }

  memset((char *) &server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(23053);

  if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind");
    exit(1);
  }

  if (listen(fd, 5) < 0) {
    perror("listen");
    closesocket(fd);
    exit(1);
  }

  //g_thread_init(NULL);
  //gdk_threads_init();
  //gdk_threads_enter();

  gtk_init(&argc, &argv);
  // TODO: absolute path
  status_icon = gtk_status_icon_new_from_file("./data/icon.png");
  gtk_status_icon_set_tooltip(status_icon, "Growl");
  gtk_status_icon_set_visible(status_icon, TRUE);
  menu = gtk_menu_new();
  g_signal_connect(GTK_STATUS_ICON(status_icon), "popup-menu", G_CALLBACK(status_icon_popup), menu);

  menu_settings = gtk_menu_item_new_with_label("Settings");
  g_signal_connect(G_OBJECT(menu_settings), "activate", G_CALLBACK(settings_clicked), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_settings);
  menu_exit = gtk_menu_item_new_with_label("Exit");
  g_signal_connect(G_OBJECT(menu_exit), "activate", G_CALLBACK(exit_clicked), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_exit);
  gtk_widget_show_all(menu);

  while (main_loop) {
    gtk_main_iteration_do(FALSE);
    FD_ZERO(&fdset);
    FD_SET(fd, &fdset);
    if (select(FD_SETSIZE, &fdset, NULL, NULL, &tv) < 0) {
      perror("select");
      continue;
    }
    if (!FD_ISSET(fd, &fdset))
      continue;
    struct sockaddr_in client;
    int sock;
    int client_len = sizeof(client);
    memset(&client, 0, sizeof(client));
    if ((sock = accept(fd, (struct sockaddr *) &client, (socklen_t *) &client_len)) < 0) {
      perror("accept");
      continue;
    }
    sockopt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &sockopt, sizeof(sockopt));

    g_thread_create(recv_thread, (gpointer) sock, FALSE, NULL);
  }
  gdk_threads_leave();

  return 0;
}

// vim:set et sw=2 ts=2 ai:
