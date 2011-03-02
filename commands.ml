
(* Class interfaces required to communicate back to the ui* modules.  The
 * respective ui* modules that use the commands class should inherit these
 * classes. The reason that they are not virtual is to provide "dummy"
 * implementations to make sure the "commands" class can have initialized
 * values... *)

class global = object
  method cmdHubOpen (_:string) = ()
end

class subject = object
  method cmdReply (_:string) = ()
end

class main = object
  inherit subject
end

class hub = object
  inherit subject
end


type from_t = Main of main | Hub of hub



class commands = object(self)
  (* initialize with dummy objects *)
  val mutable global = new global
  val mutable from = Main (new main)
  val mutable subject = (new main :> subject)
  val mutable dummy = true


  (* list of commands *)

  val mutable cmds = []
  initializer cmds <- [
    ("quit", [""; "Quit NCDC. Equivalent to hitting Ctrl+C."], self#cmdQuit);

    ("say", ["<message>";
      "Say something to the current hub or user. You normally don't have to use"
      ^" this command, you can just type your message without starting it with"
      ^" a slash."], self#cmdSay);

    ("open", ["<name>"; "Open a new tab with the given name."], self#cmdOpen);

    ("help", ["[<command>]";
      "Without argument, displays a list of all available commands.";
      "With arguments, displays usage information for the given command."],
      self#cmdHelp)
  ]


  (* implementation of the commands *)

  method private cmdQuit args =
    if List.length args > 0 then raise Exit else (
      Global.do_quit := true;
      subject#cmdReply "Quitting..."
    )

  method private cmdSay args =
    match from with
    | Main _ -> subject#cmdReply "This is not a hub nor a user."
    | Hub _ -> subject#cmdReply "Not implemented yet."

  method private cmdOpen = function
    | [n] -> global#cmdHubOpen n
    | _   -> raise Exit

  method private cmdHelp = function
    | []  -> List.iter (fun (n,_,_) -> self#replyHelp false n) cmds
    | [c] -> self#replyHelp true c
    | _   -> raise Exit


  (* Helper methods *)
  method private replyHelp usage cmd =
    try
      let _,u,_ = List.find (fun (n,_,_) -> n = cmd) cmds in
      subject#cmdReply (
        (if usage then "Usage: " else "")^
        "/"^cmd^" "^(List.hd u));
      List.iter (fun s -> subject#cmdReply ("  "^s)) (List.tl u)
    with Not_found ->
      subject#cmdReply "No such command."

  method private doCmd cmd args =
    try
      let _,_,f = List.find (fun (n,_,_) -> n = cmd) cmds in
      f args
    with Not_found|Exit ->
      self#replyHelp true cmd


  (* does nothing if str consists only of whitespace *)
  method handle str =
    if dummy then failwith "No origin set";
    let lst = Str.split (Str.regexp " +") str in
    try
      let cmd = List.hd lst in
      if cmd.[0] <> '/' then self#doCmd "say" lst
      else self#doCmd (String.sub cmd 1 (String.length cmd - 1)) (List.tl lst)
    with Failure "hd" -> ()

  method setOrigin gl fr =
    global <- gl;
    from <- fr;
    dummy <- false;
    subject <- match fr with
      | Main x -> x
      | Hub x -> x

end

