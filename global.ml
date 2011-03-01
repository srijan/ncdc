
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




(* Configuration handling.
 * Yes, the configuration is stored in marshalled hashtables. Yes, there is a
 * chance that you will lose your configuration in an OCaml update. *)

let conf_file = Filename.concat workdir "config.bin"

let conf_hash =
  try
    if Sys.file_exists conf_file then (
      let ch = open_in_bin conf_file in
      let h = (input_value ch : (string, string) Hashtbl.t) in
      close_in ch;
      h
    )
    else Hashtbl.create 30
  with _ ->
    (* silently ignore error and use empty configuration *)
    (try Unix.unlink conf_file with _ -> ());
    Hashtbl.create 30

let conf_save () =
  let ch = open_out_gen [Open_creat; Open_trunc; Open_binary] 0o600 conf_file in
  output_value ch conf_hash;
  close_out ch

let conf_l2s l = List.fold_left (fun p n -> p^"/"^n) "" l
let conf_isset l = Hashtbl.mem conf_hash (conf_l2s l)
let conf_getstr l = Hashtbl.find conf_hash (conf_l2s l)
let conf_getbool l = conf_getstr l == "true"
let conf_getint l = int_of_string (conf_getstr l)

let conf_setstr l v =
  Hashtbl.replace conf_hash (conf_l2s l) v;
  conf_save ()

let conf_setbool l v = conf_setstr l (if v then "true" else "false")
let conf_setint l v = conf_setstr l (string_of_int v)



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


