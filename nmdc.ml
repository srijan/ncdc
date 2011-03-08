
open Unix


class user name = object(self)
  val mutable isop = false
  val mutable descr = ""
  val mutable tag = ""
  val mutable connection = ""
  val mutable flags = 0
  val mutable email = ""
  val mutable share = (None : Int64.t option)

  method setOp = isop <- true
  method setMyINFO d c f m s =
    descr <- d; connection <- c; flags <- Char.code f; email <- m; share <- Some s

  method needInfo = share = None
  method getShare = match share with None -> Int64.zero | Some i -> i
end



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


(* not the most efficient methods, but oh well *)
let escape str =
  let s1 = Str.global_replace (Str.regexp "&(amp|36|124);") "&amp;\\1;" str in
  let s2 = Str.global_replace (Str.regexp "$") "&36;" s1 in
  Str.global_replace (Str.regexp "|") "&124;" s2

let unescape str =
  let s1 = Str.global_replace (Str.regexp "&36;") "$" str in
  let s2 = Str.global_replace (Str.regexp "&124;") "|" s1 in
  Str.global_replace (Str.regexp "&amp;") "&" s2



(* Hub connection states:
 !sock               -> Idle. Not connected, not connecting
 sock && !connected  -> Connecting
 sock && connected   -> Connected
*)

(* TODO: configurable character encodings *)


class hub = object(self)
  val userlist = (Hashtbl.create 100 : (string, user) Hashtbl.t)
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
  val mutable sharesize = Int64.zero
  val mutable sharecount = 0

  (* these read and write buffers are quite time-inefficient *)
  val mutable readbuf = ""
  val mutable writebuf = ""

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
  method getShareSize = (sharesize, sharecount = self#getUserCount)


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
      hubname <- unescape name
    ) with _ -> ());
    (* $Hello *)
    (try Scanf.sscanf cmd "$Hello %[^ ]" (fun nick ->
      if nick = mynick then (
        self#queueWrite "$Version 1,0091";
        self#queueWrite ("$MyINFO $ALL "^mynick^" "^mydesc^
          "<NCDC V:0.1,M:P,H:1/0/0,S:1>$ $"^myconn^"\001$"^mymail^"$0$");
        self#queueWrite "$GetNickList"
      ) else (
        Hashtbl.add userlist nick (new user nick);
        joinfunc nick
      )
    ) with _ -> ());
    (* $Quit *)
    (try Scanf.sscanf cmd "$Quit %[^ ]" (fun nick ->
      let u = Hashtbl.find userlist nick in
      if not u#needInfo then (
        sharecount <- sharecount - 1;
        sharesize <- Int64.sub sharesize u#getShare
      );
      Hashtbl.remove userlist nick;
      quitfunc nick
    ) with _ -> ());
    (* $NickList - TODO: recognise and support the NoGetINFO extension *)
    (try Scanf.sscanf cmd "$NickList %[^ ]" (fun lst ->
      List.iter (fun nick ->
        let u = try Hashtbl.find userlist nick with Not_found ->
          let n = new user nick in
          Hashtbl.add userlist nick n;
          n
        in
        if u#needInfo then self#queueWrite ("$GetINFO "^nick^" "^mynick)
      ) (Str.split (Str.regexp "\\$\\$") lst)
    ) with _ -> ());
    (* $OpList *)
    (try Scanf.sscanf cmd "$OpList %[^ ]" (fun lst ->
      List.iter (fun nick ->
        if not (Hashtbl.mem userlist nick) then
          Hashtbl.add userlist nick (new user nick);
        (Hashtbl.find userlist nick)#setOp
      ) (Str.split (Str.regexp "\\$\\$") lst)
    ) with _ -> ());
    (* $MyINFO *)
    (try Scanf.sscanf cmd "$MyINFO $ALL %[^ ] %[^$]$ $%[^$]$%[^$]$%Lu$" (fun nick d c m s ->
      try
        let f = c.[String.length c - 1] in
        let c = String.sub c 0 (String.length c - 1) in
        let u = Hashtbl.find userlist nick in
        if u#needInfo then sharecount <- sharecount + 1
        else sharesize <- Int64.sub sharesize u#getShare;
        u#setMyINFO d c f m s;
        sharesize <- Int64.add sharesize s;
      with _ -> ()
    ) with _ -> ());
    (* Main chat (everything that doesn't start with $) *)
    if String.length cmd > 0 && cmd.[0] <> '$' then
      chatfunc (unescape cmd);
    (* TODO: Search ConnectToMe To (...and more) *)

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

  method say str =
    if not connected then failwith "Not connected.";
    self#queueWrite ("<"^mynick^"> "^(escape str))

end

