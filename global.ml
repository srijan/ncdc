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



(* Writing log files *)

let curdatetime () =
  let tm = Unix.localtime (Unix.time ()) in
  Printf.sprintf "%04d-%02d-%02d %02d:%02d:%02d"
    (tm.Unix.tm_year + 1900) (tm.Unix.tm_mon + 1) (tm.Unix.tm_mday)
    (tm.Unix.tm_hour) (tm.Unix.tm_min) (tm.Unix.tm_sec)

class logfile fn =
  let chan = open_out_gen [Open_creat; Open_append; Open_text] 0o600
    (Filename.concat logdir (fn ^ ".log")) in
  let _ = output_string chan ("-------- Log starting on " ^ (curdatetime ()) ^ "\n") in
object(self)
  method write str =
    output_string chan ((curdatetime ()) ^ " " ^ str ^ "\n")
end


