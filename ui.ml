
type input_t = Key of int | Ctrl of char | Esc of string | Char of string


(* global variable to signal the mainloop to quit *)
let do_quit = ref false

(* other useful globals *)
let win_toosmall = ref false
let win_rows = ref 0
let win_cols = ref 0


external ui_getinput : unit -> input_t list = "ui_getinput"
external ui_init : unit -> unit = "ui_init"
external ui_end : unit -> unit = "ui_end"
external ui_checksize : bool ref -> int ref -> int ref -> unit = "ui_checksize"
external ui_global : string -> (string * bool) list -> unit = "ui_global"
external ui_tab_main : string array -> int -> unit = "ui_tab_main"


class virtual tab = object
  method virtual getTitle : string
  method virtual getName : string
  method virtual draw : unit (* must draw between 0 < y < wincols-2 *)
  method virtual handleInput : input_t -> bool
end



class main =
  let backlog = 1023 in (* must be 2^x-1 *)
object(self)
  inherit tab
  val log = Array.make (backlog+1) "" (* circular buffer *)
  val mutable lastlog = 0
  val mutable lastvisible = 0

  method getTitle = "Welcome to NCDC 0.1-alpha!"
  method getName = "main"

  method private addline str =
    if lastlog = lastvisible then lastvisible <- lastlog + 1;
    lastlog <- lastlog + 1;
    log.(lastlog land backlog) <- str

  method draw =
    ui_tab_main log lastvisible

  method handleInput = function
    | Key 0o522 -> (* page down *)
        lastvisible <- min (lastvisible + !win_rows/2) lastlog;
        true
    | Key 0o523 -> (* page up *)
        lastvisible <- min lastlog (max (lastvisible - !win_rows/2)
          (max (!win_rows - 4) (lastlog - backlog + !win_rows - 4)));
        true
    | _ -> false

end



class global =
  let maintab = new main in
object(self)
  val mutable tabs = ([ maintab ] : tab list)
  val mutable seltab = (maintab : tab)

  method draw =
    ui_checksize win_toosmall win_rows win_cols;
    if not !win_toosmall then begin
      ui_global seltab#getTitle (List.fold_left (fun p tab ->
        (tab#getName, tab = seltab) :: p) [] tabs);
      seltab#draw
    end

  method handleInput = function
    | Key 0o632   -> (* screen resize *)
        true
    | Ctrl '\x03' -> (* ctrl+c *)
        do_quit := true; false
    | x           -> (* not a global key, let tab handle it *)
        seltab#handleInput x

end

