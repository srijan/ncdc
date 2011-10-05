/* ncdc - NCurses Direct Connect client

  Copyright (c) 2011 Yoran Heling

  Permission is hereby granted, free of charge, to any person obtaining
  a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including
  without limitation the rights to use, copy, modify, merge, publish,
  distribute, sublicense, and/or sell copies of the Software, and to
  permit persons to whom the Software is furnished to do so, subject to
  the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/


#ifdef DOC_CMD

static const struct doc_cmd {
  char const name[16], *args, *sum, *desc;
} doc_cmds[] = {

{ "accept", NULL, "Accept the TLS certificate of a hub.",
  "Use this command to accept the TLS certificate of a hub. This command is"
  " used only in the case the keyprint of the TLS certificate of a hub does not"
  " match the keyprint stored in the configuration file."
},
{ "browse", "[[-f] <user>]", "Download and browse someone's file list.",
  "Without arguments, this opens a new tab where you can browse your own file list."
  " Note that changes to your list are not immediately visible in the browser."
  " You need to re-open the tab to get the latest version of your list.\n\n"
  "With arguments, the file list of the specified user will be downloaded (if"
  " it has not been downloaded already) and the browse tab will open once it's"
  " complete. The `-f' flag can be used to force the file list to be (re-)downloaded."
},
{ "clear", NULL, "Clear the display.",
  "Clears the log displayed on the screen. Does not affect the log files in any way."
  " Ctrl+l is a shortcut for this command."
},
{ "close", NULL, "Close the current tab.",
  "Close the current tab. When closing a hub tab, you will be disconnected from"
  " the hub and all related userlist and PM tabs will also be closed. Alt+c is"
  " a shortcut for this command."
},
{ "connect", "[<address>]", "Connect to a hub.",
  "Initiate a connection with a hub. If no address is specified, will connect"
  " to the hub you last used on the current tab. The address should be in the"
  " form of `protocol://host:port/' or `host:port'. The `:port' part is in both"
  " cases optional and defaults to :411. The following protocols are"
  " recognized: dchub, nmdc, nmdcs, adc, adcs. When connecting to an nmdcs or"
  " adcs hub and the SHA256 keyprint is known, you can attach this to the url as"
  " `?kp=SHA256/<base32-encoded-keyprint>'\n\n"
  "Note that this command can only be used on hub tabs. If you want to open a new"
  " connection to a hub, you need to use /open first. For example:\n"
  "  /open testhub\n"
  "  /connect dchub://dc.some-test-hub.com/\n"
  "See the /open command for more information."
},
{ "connections", NULL, "Open the connections tab.",
  NULL
},
{ "disconnect", NULL, "Disconnect from a hub.",
  NULL
},
{ "gc", NULL, "Perform some garbage collection.",
  "Cleans up unused data and reorganizes existing data to allow more efficient"
  " storage and usage. Currently, this commands cleans up hashdata.dat and"
  " dl.dat, removes unused files in inc/ and old files in fl/.\n\n"
  "This command may take some time to complete, and will fully block ncdc while"
  " it is running. You won't have to perform this command very often."
},
{ "grant", "[<user>]", "Grant someone a slot.",
  "Grant someone a slot. This allows the user to download from you even if you"
  " have no free slots.  The slot will be granted for as long as ncdc stays"
  " open. If you restart ncdc, the user will have to wait for a regular slot."
  " Unless, of course, you /grant a slot again.\n\n"
  "Note that a granted slot is specific to a single hub. If the same user is"
  " also on other hubs, he/she will not be granted a slot on those hubs."
},
{ "help", "[<command>|set <key>|keys [<section>]]", "Request information on commands.",
  "To get a list of available commands, use /help without arguments.\n"
  "To get information on a particular command, use /help <command>.\n"
  "To get information on a configuration setting, use /help set <setting>.\n"
  "To get help on key bindings, use /help keys.\n"
},
{ "kick", "<user>", "Kick a user from the hub.",
  "Kick a user from the hub. This command only works on NMDC hubs, and you need"
  " to be an OP to be able to use it."
},
{ "me", "<message>", "Chat in third person.",
  "This allows you to talk in third person. Most clients will display your message as something like:\n"
  "  ** Nick is doing something\n\n"
  "Note that this command only works correctly on ADC hubs. The NMDC protocol"
  " does not have this feature, and your message will be sent as-is, including the /me."
},
{ "msg", "<user> [<message>]", "Send a private message.",
  "Send a private message to a user on the currently opened hub. If no"
  " message is given, the tab will be opened but no message will be sent."
},
{ "nick", "[<nick>]", "Alias for `/set nick'.",
  NULL
},
{ "open", "[-n] <name> [<address>]", "Open a new hub tab and connect to the hub.",
  "Opens a new tab to use for a hub. The name is a (short) personal name you"
  " use to identify the hub, and will be used for storing hub-specific"
  " configuration.\n\n"
  "If you have specified an address or have previously connected to a hub"
  " from a tab with the same name, /open will automatically connect to"
  " the hub. Use the `-n' flag to disable this behaviour.\n\n"
  "See /connect for more information on connecting to a hub."
},
{ "password", "<password>", "Send your password to the hub.",
  "This command can be used to send a password to the hub without saving it to"
  " the config file. If you wish to login automatically without having to type"
  " /password every time, use '/set password <password>'. Be warned, however,"
  " that your password will be saved unencrypted in that case."
},
{ "pm", "<user> [<message>]", "Alias for /msg",
  NULL
},
{ "queue", NULL, "Open the download queue.",
  NULL
},
{ "quit", NULL, "Quit ncdc.",
  "Quit ncdc. You can also just hit ctrl+c, which is equivalent."
},
{ "reconnect", NULL, "Shortcut for /disconnect and /connect",
  "Reconnect to the hub. When your nick or the hub encoding have been changed,"
  " the new settings will be used after the reconnect.\n\n"
  "This command can also be used on the main tab, in which case all connected"
  " hubs will be reconnected."
},
{ "refresh", "[<path>]", "Refresh file list.",
  "Initiates share refresh. If no argument is given, the complete list will be"
  " refreshed. Otherwise only the specified directory will be refreshed. The"
  " path argument can be either an absolute filesystem path or a virtual path"
  " within your share."
},
{ "say", "<message>", "Send a chat message.",
  "Sends a chat message to the current hub or user. You normally don't have to"
  " use the /say command explicitly, any command not staring with '/' will"
  " automatically imply `/say <command>'. For example, typing `hello.' in the"
  " command line is equivalent to `/say hello.'. Using the /say command"
  " explicitly may be useful to send message starting with '/' to the chat, for"
  " example `/say /help is what you are looking for'."
},
{ "search", "[options] <query>", "Search for files.",
  "Performs a file search, opening a new tab with the results.\n\n"
  "Available options:\n"
  "  -hub      Search the current hub only. (default)\n"
  "  -all      Search all connected hubs.\n"
  "  -le  <s>  Size of the file must be less than <s>.\n"
  "  -ge  <s>  Size of the file must be larger than <s>.\n"
  "  -t   <t>  File must be of type <t>. (see below)\n"
  "  -tth <h>  TTH root of this file must match <h>.\n\n"
  "File sizes (<s> above) accept the following suffixes: G (GiB), M (MiB) and K (KiB).\n\n"
  "The following file types can be used with the -t option:\n"
  "  1  any      Any file or directory. (default)\n"
  "  2  audio    Audio files.\n"
  "  3  archive  (Compressed) archives.\n"
  "  4  doc      Text documents.\n"
  "  5  exe      Windows executables.\n"
  "  6  img      Image files.\n"
  "  7  video    Video files.\n"
  "  8  dir      Directories.\n"
  "Note that file type matching is done using file extensions, and is not very reliable."
},
// TODO: document that some settings can be set on a per-hub basis?
{ "set", "[<key> [<value>]]", "Get or set configuration variables.",
  "Get or set configuration variables. Use without arguments to get a list of "
  " all settings and their current value. Changes to the settings are"
  " automatically saved to the config file, and will not be lost after"
  " restarting ncdc.\n\n"
  "To get information on a particular setting, use `/help set <key>'."
},
{ "share", "[<name> <path>]", "Add a directory to your share.",
  "Use /share without arguments to get a list of shared directories.\n"
  "When called with a name and a path, the path will be added to your share."
  " Note that shell escaping may be used in the name. For example, to add a"
  " directory with the name `Fun Stuff', you could do the following:\n"
  "  /share \"Fun Stuff\" /path/to/fun/stuff\n"
  "Or:\n"
  "  /share Fun\\ Stuff /path/to/fun/stuff\n\n"
  "The full path to the directory will not be visible to others, only the name"
  " you give it will be public. An initial `/refresh' is done automatically on"
  " the added directory."
},
{ "unset", "[<key>]", "Unset a configuration variable.",
  "This command can be used to reset a configuration variable back to its default value."
},
{ "unshare", "[<name>]", "Remove a directory from your share.",
  "To remove a single directory from your share, use `/unshare <name>', to"
  " remove all directories from your share, use `/unshare /'.\n\n"
  "Note that all hash data for the removed directories will be thrown away. All"
  " files will have to be re-hashed again when the directory is later re-added."
},
{ "userlist", NULL, "Open the user list.",
  "Opens the user list of the currently selected hub. Can also be accessed using Alt+u."
},
{ "version", NULL, "Display version information.",
  NULL
},
{ "whois", "<user>", "Locate a user in the user list.",
  "This will open the user list and select the given user."
},

{ "" }
};

#endif // DOC_CMD



#ifdef DOC_SET

static const struct doc_set {
  char const *name, *type, *desc;
} doc_sets[] = {

{ "active", "<boolean>",
  "Enables or disables active mode. Make sure to set `active_ip' and"
  " `active_port' before enabling active mode."
},
{ "active_bind", "<string>",
  "IP address to bind to in active mode. When unset, ncdc will bind to all"
  " interfaces."
},
{ "active_ip", "<string>",
  "Your public IP address for use in active mode. It is important that other"
  " clients can reach you using this IP address. If you connect to a hub on the"
  " internet, this should be your internet (WAN) IP. Likewise, if you connect"
  " to a hub on your LAN, this should be your LAN IP.\n\n"
  "Note that this setting is global for ncdc: it is currently not possible to"
  " use a single instance of ncdc to connect to both internet and LAN hubs, if"
  " you are not reachable on the same IP with both networks. In that case you can"
  " either use passive mode or run two separate instances of ncdc."
},
{ "active_port", "<integer>",
  "The listen port for incoming connections in active mode. Set to `0' to"
  " automatically assign a random port. If TLS support is available, another"
  " TCP port will be opened on the configured port + 1. Ncdc will tell you"
  " exactly on which ports it is listening for incoming packets. If you are"
  " behind a router or firewall, make sure that you have configured it to"
  " forward and allow these ports."
},
{ "autoconnect", "<boolean>",
  "Set to true to automatically connect to the current hub when ncdc starts up."
},
{ "autorefresh", "<integer>",
  "The time between automatic file refreshes, in minutes. Set to 0 to disable"
  " automatically refreshing the file list. This setting also determines"
  " whether ncdc will perform a refresh on startup. See the `/refresh' command to"
  " manually refresh your file list."
},
{ "backlog", "<integer>",
  "When opening a hub or PM tab, ncdc can load a certain amount of lines from"
  " the log file into the log window. Setting this to a positive value enables"
  " this feature and configures the number of lines to load. Note that, while"
  " this setting can be set on a per-hub basis, PM windows will use the global"
  " value (global.backlog)."
},
// Note: the setting list isn't alphabetic here, but in a more intuitive order
{ "color_*", "<color>",
  "The settings starting with the `color_' prefix allow you to change the"
  " interface colors. The following is a list of available color settings:\n"
  "  log_default   - default log color\n"
  "  log_time      - the time prefix in log messages\n"
  "  log_nick      - default nick color\n"
  "  log_highlight - nick color of a highlighted line\n"
  "  log_ownnick   - color of your own nick\n"
  "  log_join      - color of join messages\n"
  "  log_quit      - color of quit messages\n"
  "  tabprio_low   - low priority tab notification color\n"
  "  tabprio_med   - medium priority tab notification color\n"
  "  tabprio_high  - high priority tab notification color\n"
  "\n"
  "The actual color value can be set with a comma-separated list of color names"
  " and/or attributes. The first color in the list is the foreground color, the"
  " second color is used for the background. When the fore- or background color"
  " is not specified, the default colors of your terminal will be used.\n"
  "The following color names can be used: black, blue, cyan, default, green,"
  " magenta, red, white and yellow.\n"
  "The following attributes can be used: bold, reverse and underline.\n"
  "The actual color values displayed by your terminal may vary. Adding the"
  " `bold' attribute usually makes the foreground color appear brighter as well."
},
{ "connection", "<string>",
  "Set your upload speed. This is just an indication for other users in the hub"
  " so that they know what speed they can expect when downloading from you. The"
  " actual format you can use here may vary, but it is recommended to set it to"
  " either a plain number for Mbit/s (e.g. `50' for 50 mbit) or a number with a"
  " `KiB/s' indicator (e.g. `2300 KiB/s'). On ADC hubs you must use one of the"
  " previously mentioned formats, otherwise no upload speed will be"
  " broadcasted. This setting is broadcasted as-is on NMDC hubs, to allow for"
  " using old-style connection values (e.g. `DSL' or `Cable') on hubs that"
  " require this."
},
{ "description", "<string>",
  "A short public description that will be displayed in the user list of a hub."
},
{ "download_dir", "<path>",
  "The directory where finished downloads are moved to. Finished downloads are"
  " by default stored in <session directory>/dl/. It is possible to set this to"
  " a location that is on a different filesystem than the incoming directory,"
  " but doing so is not recommended: ncdc will block when moving the completed"
  " files to their final destination."
},
{ "download_exclude", "<regex>",
  "When recursively adding a directory to the download queue - by pressing `b'"
  " on a directory in the file list browser - any item in the selected"
  " directory with a name that matches this regular expression will not be"
  " added to the download queue.\n\n"
  "This regex is not checked when adding individual files from either the file"
  " list browser or the search results."
},
{ "download_slots", "<integer>",
  "Maximum number of simultaneous downloads."
},
{ "email", "<string>",
  "Your email address. This will be displayed in the user list of the hub, so"
  " only set this if you want it to be public."
},
{ "encoding", "<string>",
  "The character set/encoding to use for hub and PM messages. This setting is"
  " only used on NMDC hubs, ADC always uses UTF-8. Some common values are:\n"
  "  CP1250      (Central Europe)\n"
  "  CP1251      (Cyrillic)\n"
  "  CP1252      (Western Europe)\n"
  "  ISO-8859-7  (Greek)\n"
  "  KOI8-R      (Cyrillic)\n"
  "  UTF-8       (International)"
},
{ "hubname", "<string>",
  "The name of the currently opened hub tab. This is a user-assigned name, and"
  " is only used within ncdc itself. This is the same name as given to the"
  " `/open' command."
},
{ "incoming_dir", "<path>",
  "The directory where incomplete downloads are stored. This setting can only"
  " be changed when the download queue is empty. Also see the download_dir"
  " setting."
},
{ "log_debug", "<boolean>",
  "Log debug messages to stderr.log in the session directory. It is highly"
  " recommended to enable this setting if you wish to debug or hack ncdc. Be"
  " warned, however, that this may generate a lot of data if you're connected"
  " to a large hub."
},
{ "log_downloads", "<boolean>",
  "Log downloaded files to transfers.log."
},
{ "log_uploads", "<boolean>",
  "Log file uploads to transfers.log."
},
{ "minislots", "<integer>",
  "Set the number of available minislots. A `minislot' is a special slot that"
  " is used when all regular upload slots are in use and someone is requesting"
  " your filelist or a small file. In this case, the other client automatically"
  " applies for a minislot, and can still download from you as long as not all"
  " minislots are in use. What constitutes a `small' file can be changed with"
  " the `minislot_size' setting. Also see the `slots' configuration setting and"
  " the `/grant' command."
},
{ "minislot_size", "<integer>",
  "The maximum size of a file that may be downloaded using a `minislot', in"
  " KiB. See the `minislots' setting for more information."
},
{ "nick", "<string>",
  "Your nick. Nick changes are only visible on newly connected hubs, use the "
  " `/reconnect' command to use your new nick immediately. Note that it is"
  " highly discouraged to change your nick on NMDC hubs. This is because"
  " clients downloading from you have no way of knowing that you changed your"
  " nick, and therefore can't immediately continue to download from you."
},
{ "password", "<string>",
  "Sets your password for the current hub and enables auto-login on connect. If"
  " you just want to login to a hub without saving your password, use the"
  " `/password' command instead. Passwords are saved unencrypted in the config"
  " file."
},
{ "share_exclude", "<regex>",
  "Any file or directory with a name that matches this regular expression will"
  " not be shared. A file list refresh is required for this setting to be"
  " effective."
},
{ "share_hidden", "<boolean>",
  "Whether to share hidden files and directories. A `hidden' file or directory"
  " is one of which the file name starts with a dot. (e.g. `.bashrc'). A file"
  " list refresh is required for this setting to be effective."
},
{ "show_joinquit", "<boolean>",
  "Whether to display join/quit messages in the hub chat."
},
{ "slots", "<integer>",
  "The number of upload slots. This determines for the most part how many"
  " people can download from you simultaneously. It is possible that this limit"
  " is exceeded in certain circumstances, see the `minislots' setting and the"
  " `/grant' command."
},
{ "tls_policy", "<disabled|allow|prefer>",
  "Set the policy for secure client-to-client connections. Setting this to"
  " `disabled' disables TLS support for client connections, but still allows"
  " you to connect to TLS-enabled hubs. `allow' will allow the use of TLS if"
  " the other client requests this, but ncdc itself will not request TLS when"
  " connecting to others. Setting this to `prefer' tells ncdc to also request"
  " TLS when connecting to others.\n\n"
  "The use of TLS for client connections usually results in less optimal"
  " performance when uploading and downloading, but is quite effective at"
  " avoiding protocol-specific traffic shaping that some ISPs may do. Also note"
  " that, even if you set this to `prefer', TLS will only be used if the"
  " connecting party also supports it."
},
{ "ui_time_format", "<string>",
  "The format of the time displayed in the lower-left of the screen. Set `-' to"
  " not display a time at all. The string is passed to the Glib"
  " g_date_time_format() function, which accepts roughly the same formats as"
  " strftime(). Check out the strftime(3) man page or the Glib documentation"
  " for more information. Note that this setting does not influence the"
  " date/time format used in other places, such as the chat window or log"
  " files."
},

{ NULL }
};

#endif // DOC_SET



#ifdef DOC_KEY

// There is some redundancy here with the actual keys used in the switch()
// statements of each window/widget. But the methods for synchronizing the keys
// aren't going to worth it I'm afraid.

// Don't want to redirect people to the global keybindings for these four lines.
#define LISTING_KEYS \
  "Up/Down      Select one item up/down.\n"\
  "k/j          Select one item up/down.\n"\
  "PgUp/PgDown  Select one page of items up/down.\n"\
  "End/Home     Select last/first item in the list.\n"

static const struct doc_key {
  char const *sect, *title, *desc;
} doc_keys[] = {

{ "global", "Global key bindings",
  "Alt+j        Open previous tab.\n"
  "Alt+k        Open next tab.\n"
  "Alt+h        Move current tab left.\n"
  "Alt+l        Move current tab right.\n"
  "Alt+<num>    Open tab with number <num>.\n"
  "Alt+c        Close current tab.\n"
  "Alt+n        Open the connections tab.\n"
  "Alt+q        Open the download queue tab.\n"
  "Alt+o        Open own file list.\n"
  "Alt+r        Refresh file list.\n"
  "Ctrl+c       Quit ncdc.\n"
  "\n"
  "Keys for tabs with a log window:\n"
  "Ctrl+l       Clear current log window.\n"
  "PgUp         Scroll the log backward.\n"
  "PgDown       Scroll the log forward.\n"
  "\n"
  "Keys for tabs with a text input line:\n"
  "Left/Right   Move cursor one character left or right.\n"
  "End/Home     Move cursor to the end / start of the line.\n"
  "Up/Down      Scroll through the command history.\n"
  "Tab          Auto-complete current command, nick or argument.\n"
  "Alt+b        Move cursor one word backward.\n"
  "Alt+f        Move cursor one word forward.\n"
  "Backspace    Delete character before cursor.\n"
  "Delete       Delete character under cursor.\n"
  "Ctrl+w       Delete to previous space.\n"
  "Alt+d        Delete to next space.\n"
  "Ctrl+k       Delete everything after cursor.\n"
  "Ctrl+u       Delete entire line."
},
{ "browse", "File browser",
  LISTING_KEYS
  "Right/l      Open selected directory.\n"
  "Left/h       Open parent directory.\n"
  "d            Add selected file/directory to the download queue."
},
{ "connections", "Connection list",
  LISTING_KEYS
  "d            Disconnect selected connection.\n"
  "i/Return     Toggle information box.\n"
  "f            Find user in user list.\n"
  "q            Find file in download queue."
},
{ "queue", "Download queue",
  LISTING_KEYS
  "f            Find user in user list.\n"
  "c            Find connection in the connection list.\n"
  "d            Remove selected file from the queue.\n"
  "+/-          Increase/decrease priority.\n"
  "\n"
  "Note: when an item in the queue has `ERR' indicated in the priority column,"
  " you have two choices: You can remove the item from the queue using `d', or"
  " attempt to continue the download by increasing its priority using `+'."
},
{ "search", "Search results tab",
  LISTING_KEYS
  "f            Find user in user list.\n"
  "b/B          Browse the selected users' list, B to force a redownload.\n"
  "h            Toggle hub column visibility.\n"
  "u            Order by username.\n"
  "s            Order by file size.\n"
  "l            Order by free slots.\n"
  "n            Order by file name."
},
{ "userlist", "User list tab",
  LISTING_KEYS
  "o            Toggle sorting OPs before others.\n"
  "s/S          Order by share size.\n"
  "u/U          Order by username.\n"
  "t/T          Toggle visibility / order by tag column.\n"
  "e/E          Toggle visibility / order by email column.\n"
  "c/C          Toggle visibility / order by connection column.\n"
  "p/P          Toggle visibility / order by IP column.\n"
  "i/Return     Toggle information box.\n"
  "m            Send a PM to the selected user.\n"
  "g            Grant a slot to the selected user.\n"
  "b/B          Browse the selected users' list, B to force a redownload."
},

{ NULL }
};

#undef LISTING_KEYS

#endif // DOC_KEY
