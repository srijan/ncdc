
(* global variable to signal the mainloop to do something *)
let do_quit = ref false
let do_redraw = ref false



(* This initialization code is executed before anything else has been
 * initialized, so if we let things fail here the program will refuse to run.
 * (which is what we want if the working directory is not sane) *)


(* Figure out our working directory. *)
let workdir =
  try
    Sys.getenv "NCDC_DIR"
  with Not_found -> try
    Filename.concat (Sys.getenv "HOME") ".ncdc"
  with Not_found -> ".ncdc"

(* log directory *)
let logdir = Filename.concat workdir "logs"

(* create directories if they don't exist yet and check for sanity *)
let _ =
  try
    if not (Sys.file_exists workdir) then Unix.mkdir workdir 0o700;
    if not (Sys.file_exists logdir) then Unix.mkdir logdir 0o700;
    Unix.access workdir [Unix.R_OK; Unix.W_OK; Unix.X_OK; Unix.F_OK];
    Unix.access logdir [Unix.R_OK; Unix.W_OK; Unix.X_OK; Unix.F_OK]
  with _ ->
    prerr_endline ("Unable to use '" ^ workdir ^ "' as working directory.");
    prerr_endline "Hint: check whether its parent directory exists and you have enough access to it.";
    exit 0




(* Configuration handling. *)

module Conf = struct
  let file = Filename.concat workdir "config"
  let db = Dbm.opendbm file [Dbm.Dbm_rdwr; Dbm.Dbm_create] 0o600
  let close () = Dbm.close db

  (* print out configuration and exit when NCDC_CONF_DEBUG is set *)
  let _ = try
    ignore (Sys.getenv "NCDC_CONF_DEBUG");
    Dbm.iter (fun k v -> print_endline (k^" "^v)) db;
    exit 0
  with _ -> ()

  (* low-level functions *)
  let l2s l = String.concat "/" l
  let get l = Dbm.find db (l2s l)
  let set l v = Dbm.replace db (l2s l) v

  let rec gethubopt h o d = match h with
    | None -> (try get [o] with _ -> d)
    | Some hub -> try get [hub; o] with _ -> gethubopt None o d
  let sethubopt h o v = match h with
    | None -> set [o] v
    | Some hub -> set [hub; o] v

  (* high-level functions *)
  let getnick h   = gethubopt h "nick" ("NCDC_"^(string_of_int (Random.int 10000)))
  let setnick h n =
    if Str.string_match (Str.regexp "[$| ]") n 0 then
      invalid_arg "Invalid character in nick."
    else if String.length n < 1 || String.length n > 32 then
      invalid_arg "Nick must be between 1 and 32 characters."
    else sethubopt h "nick" n

  let getemail h   = gethubopt h "email" ""
  let setemail h e =
    if Str.string_match (Str.regexp "[$|]") e 0 then
      invalid_arg "Invalid character in email address."
    else sethubopt h "email" e

  let getdescription h   = gethubopt h "description" ""
  let setdescription h d =
    if Str.string_match (Str.regexp "[$|]") d 0 then
      invalid_arg "Invalid character in description."
    else sethubopt h "description" d

  let getconnection h   = gethubopt h "connection" ""
  let setconnection h c =
    if Str.string_match (Str.regexp "[$|]") c 0 then
      invalid_arg "Invalid character in connection."
    else sethubopt h "connection" c

  (* make sure the generated nick is at least persistent *)
  let _ = setnick None (getnick None)
end



(* Writing log files *)

class logfile fn =
  let chan = open_out_gen [Open_creat; Open_append; Open_text] 0o600
    (Filename.concat logdir (fn ^ ".log")) in
object(self)
  method datestr =
    let tm = Unix.localtime (Unix.time ()) in
    Printf.sprintf "%04d-%02d-%02d %02d:%02d:%02d"
      (tm.Unix.tm_year + 1900) (tm.Unix.tm_mon + 1) (tm.Unix.tm_mday)
      (tm.Unix.tm_hour) (tm.Unix.tm_min) (tm.Unix.tm_sec)

  method write str =
    output_string chan (self#datestr ^ " " ^ str ^ "\n");
    flush chan

  initializer
    output_string chan ("-------- Log starting on " ^ self#datestr ^ "\n")
end


