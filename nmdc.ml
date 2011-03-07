
open Unix



(* http://www.teamfair.info/wiki/index.php?title=Lock_to_key
 * No, I didn't steal it, I was the one who contributed this function. ^-^ *)
let lock2key lock =
  let len = String.length lock in
  let key = Array.make len 0 in
  let l n = Char.code lock.[n] in
  Array.iteri (fun i n -> key.(i) <-
    if i == 0 then l i lxor l (len-1) lxor l (len-2) lxor 5
    else           l i lxor l (i-1)
  ) key;
  Array.fold_left (fun str n ->
    str ^ match ((n lsl 4) lor (n lsr 4)) land 255 with
    |   0 -> "/%DCN000%/" |   5 -> "/%DCN005%/" |  36 -> "/%DCN036%/"
    |  96 -> "/%DCN096%/" | 124 -> "/%DCN124%/" | 126 -> "/%DCN126%/"
    |   c -> String.make 1 (Char.chr c)
  ) "" key



(* Hub connection states:
 !sock               -> Idle. Not connected, not connecting
 sock && !connected  -> Connecting
 sock && connected   -> Connected
*)

(* TODO:
 - escape/unescape special characters in hub name and chat
 - configurable character encodings
*)


class hub = object(self)
  val userlist = (Hashtbl.create 100 : (string, bool) Hashtbl.t)
  val mutable sock = (None : file_descr option)
  val mutable connected = false

  val mutable disconnectfunc = (fun () -> ())
  val mutable iofunc = ((fun _ _ -> ()) : bool -> string -> unit)
  val mutable joinfunc = ((fun _ -> ()) : string -> unit)
  val mutable quitfunc = ((fun _ -> ()) : string -> unit)
  val mutable chatfunc = ((fun _ -> ()) : string -> unit)

  val mutable mynick = ""
  val mutable mydesc = ""
  val mutable mymail = ""
  val mutable myconn = ""
  val mutable addr = inet_addr_any
  val mutable port = 411
  val mutable hubname = ""

  (* these read and write buffers are quite time-inefficient *)
  val mutable readbuf = ""
  val mutable writebuf = ""

  (* TODO: More callbacks? Or just an object reference? (as with the commands) *)
  method setDisconnectFunc f = disconnectfunc <- f
  method setIOFunc f      = iofunc <- f
  method setJoinFunc f    = joinfunc <- f
  method setQuitFunc f    = quitfunc <- f
  method setChatFunc f    = chatfunc <- f
  method setNick n        = mynick <- n
  (* TODO: re-send $MyINFO on change *)
  method setDescription d = mydesc <- d
  method setEmail e       = mymail <- e
  method setConnection c  = myconn <- c

  method getAddr = string_of_inet_addr addr
  method getPort = port
  method getNick = mynick
  method getUserList = userlist
  method getUserCount = Hashtbl.length userlist
  method isConnected = connected
  method isConnecting = not connected && sock <> None
  method getHubName = hubname


  method setSelectMasks rd wr =
    try
      let s = self#getSock in
      if connected then Hashtbl.add rd s self#doRead;
      if connected && writebuf <> "" then Hashtbl.add wr s self#doWrite;
      if not connected then Hashtbl.add wr s self#handleConnect;
    with Failure "Not connected" -> ()

  method private getSock =
    match sock with
    | None -> failwith "Not connected"
    | Some s -> s

  method private queueWrite str =
    iofunc false str;
    writebuf <- writebuf ^ str ^ "|"

  method private doWrite () =
    let len = String.length writebuf in
    let cnt = send self#getSock writebuf 0 len [] in
    if cnt == 0 then self#disconnect;
    writebuf <-
      if cnt = len then ""
      else String.sub writebuf cnt (len-cnt)

  method private doRead () =
    let buf = String.create 8192 in
    let cnt = recv self#getSock buf 0 (String.length buf) [] in
    if cnt == 0 then self#disconnect;
    readbuf <- readbuf ^ (String.sub buf 0 cnt);
    while (String.contains readbuf '|') do (
      let i = String.index readbuf '|' in
      let cmd = String.sub readbuf 0 i in
      readbuf <- if String.length readbuf <= i-1 then ""
        else String.sub readbuf (i+1) (String.length readbuf-i-1);
      self#handleCmd cmd
    ) done

  method private handleCmd cmd =
    iofunc true cmd;
    (* $Lock *)
    (try Scanf.sscanf cmd "$Lock %[^ $] Pk=%[^ $]" (fun lock _ ->
      self#queueWrite ("$Key " ^ (lock2key lock));
      self#queueWrite ("$ValidateNick " ^ mynick)
    ) with _ -> ());
    (* $HubName *)
    (try Scanf.sscanf cmd "$HubName %[^|]" (fun name ->
      hubname <- name
    ) with _ -> ());
    (* $Hello *)
    (try Scanf.sscanf cmd "$Hello %[^ ]" (fun nick ->
      if nick = mynick then (
        self#queueWrite "$Version 1,0091";
        self#queueWrite ("$MyINFO $ALL "^mynick^" "^mydesc^
          "<NCDC V:0.1,M:P,H:1/0/0,S:1>$ $"^myconn^"\001$"^mymail^"$0$");
        self#queueWrite "$GetNickList"
      ) else (
        Hashtbl.add userlist nick false;
        joinfunc nick
      )
    ) with _ -> ());
    (* $Quit *)
    (try Scanf.sscanf cmd "$Quit %[^ ]" (fun nick ->
      Hashtbl.remove userlist nick;
      quitfunc nick
    ) with _ -> ());
    (* $NickList *)
    (try Scanf.sscanf cmd "$NickList %[^ ]" (fun lst ->
      List.iter
        (fun nick -> Hashtbl.add userlist nick false)
        (Str.split (Str.regexp "\\$\\$") lst)
    ) with _ -> ());
    (* Main chat (everything that doesn't start with $) *)
    if String.length cmd > 0 && cmd.[0] <> '$' then
      chatfunc cmd;
    (* TODO: MyINFO Search ConnectToMe To (...and more) *)

  method connect host p =
    if sock <> None then failwith "Already connected/connecting.";
    (* blocking gethostbyname :-( *)
    addr <- (gethostbyname host).h_addr_list.(0);
    port <- p;
    let s = socket PF_INET SOCK_STREAM 0 in
    sock <- Some s;
    (* assumes we're on a system that supports non-blocking connect *)
    set_nonblock s;
    try
      ignore (connect s (ADDR_INET (addr, port)));
      self#handleConnect ()
    with Unix_error (EINPROGRESS, _, _) -> ()

  method private handleConnect () =
    let s = self#getSock in
    match getsockopt_error s with
    | None -> clear_nonblock s; connected <- true
    | Some e -> self#disconnect

  method disconnect =
    close self#getSock;
    connected <- false;
    sock <- None;
    Hashtbl.clear userlist;
    hubname <- "";
    disconnectfunc ()

end

