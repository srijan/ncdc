

(* Commandlist.
 *  ($commandname, $usage, $fun), ..
 *  $fun: (arguments : string list) -> (return : string)
 *  return = "" -> "/$commandname $usage"
 *)

let rec cmdlist = [
  ("quit", [""; "  Quit NCDC. Equivalent to hitting Ctrl+C."], (fun args ->
    if List.length args > 0 then [] else (
      Global.do_quit := true;
      ["Quitting..."]
    )
  ));

  ("say", ["<text>"; "  Say something to the current hub or user."], (fun args ->
    ["Not implemented yet."]
  ));

  ("help", ["[<command>]";
    "  Without argument, displays a list of all available commands.";
    "  With arguments, displays usage information for the given command."],
    (fun args ->
    match args with
    | [] -> (* list all commands *)
      List.fold_left (fun p (c, u, _) ->
        p @ (("/"^c^" "^(List.hd u)) :: List.tl u)
      ) [] cmdlist
    | [c] -> (* help for a single command (can't use the cmd_hash here T.T) *)
      (try
        let (_,u,_) = List.find (fun (n,_,_) -> n = c) cmdlist in
        ("Usage: /"^c^" "^(List.hd u)) :: List.tl u
      with Not_found -> ["No such command."])
    | _ -> []
  ));
]


and docmd cmd args =
  let (_, u, f) =
    try List.find (fun (n,_,_) -> n = cmd) cmdlist
    with Not_found -> ("", [""], (fun _ -> ["No such command."]))
  in
  let r = f args in
  if List.length r < 1 then docmd "help" [cmd] else r


let handle str =
  let lst = Str.split (Str.regexp " +") str in
  let (cmd, args) = match lst with
  | h :: t -> (h, t)
  | _      -> ("/'", [])
  in
  if cmd.[0] <> '/' then docmd "say" lst
  else docmd (String.sub cmd 1 (String.length cmd - 1)) args


