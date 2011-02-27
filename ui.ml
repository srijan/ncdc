
type input_t = Key of int | Ctrl of char | Esc of string | Char of string


(* global variable to signal the mainloop to do something *)
let do_quit = ref false
let do_redraw = ref false

(* other useful globals *)
let win_toosmall = ref false
let win_rows = ref 0
let win_cols = ref 0


class virtual tab = object
  method virtual init : unit
  method virtual getTitle : string
  method virtual getName : string
  method virtual draw : unit (* must draw between 0 < y < wincols-2 *)
  method virtual handleInput : input_t -> bool
end


external ui_getinput : unit -> input_t list = "ui_getinput"
external ui_init : unit -> unit = "ui_init"
external ui_end : unit -> unit = "ui_end"
external ui_refresh : unit -> unit = "ui_refresh"
external ui_textinput_get : string -> string = "ui_textinput_get"
external ui_textinput_set : string -> string ref -> int ref -> unit = "ui_textinput_set"
external ui_textinput_key : input_t -> string ref -> int ref -> bool = "ui_textinput_key"
external ui_textinput_draw : (int * int * int) -> string -> int -> unit = "ui_textinput_draw"
external ui_checksize : bool ref -> int ref -> int ref -> unit = "ui_checksize"
external ui_global : tab list -> tab -> unit = "ui_global"
external ui_tab_main : (float * string) array -> int -> unit = "ui_tab_main"



(* minimal text input "widget".
 * Has to be written entirely in C to get unicode support working.
 * TODO: history and tab completion support *)
class textInput = object(self)
  (* these are used for storage in C, may be garbage in OCaml *)
  val curpos = ref 0 (* character count, not bytes nor columns *)
  val str = ref (String.make 32 '\x00') (* wchar_t* in C *)

  method getText = ui_textinput_get !str
  method setText a = ui_textinput_set a str curpos
  method draw y x cols = ui_textinput_draw (y, x, cols) !str !curpos
  method handleInput t = ui_textinput_key t str curpos
end



(* TODO: scrolling the log should work with screen lines, not log entries *)
class main =
  let backlog = 1023 in (* must be 2^x-1 *)
object(self)
  inherit tab
  val log = Array.make (backlog+1) (0.0, "") (* circular buffer *)
  val logf = new Global.logfile "main"
  val cmd = new textInput;
  val mutable lastlog = 0
  val mutable lastvisible = 0

  method getTitle = "Welcome to NCDC 0.1-alpha!"
  method getName = "main"

  method private addline str =
    if lastlog = lastvisible then lastvisible <- lastlog + 1;
    lastlog <- lastlog + 1;
    log.(lastlog land backlog) <- (Unix.time (), str);
    logf#write str

  method private drawCmd =
    cmd#draw (!win_rows-3) 9 (!win_cols-9)

  method init =
    self#addline "NCDC 0.1-alpha starting up...";
    self#addline ("Using working directory: " ^ Global.workdir)

  method draw =
    ui_tab_main log lastvisible;
    self#drawCmd (* must be last to have the cursor at the correct position *)

  method handleInput = function
    | Key 0o522 -> (* page down *)
        lastvisible <- min (lastvisible + !win_rows/2) lastlog;
        true
    | Key 0o523 -> (* page up *)
        lastvisible <- min lastlog (max (lastvisible - !win_rows/2)
          (max (!win_rows - 4) (lastlog - backlog + !win_rows - 4)));
        true
    | Ctrl '\n' -> (* return *)
        self#addline (cmd#getText);
        cmd#setText "";
        true
    | k ->
        if cmd#handleInput k then self#drawCmd;
        false

end



class global =
  let maintab = (new main :> tab) in
  let _ = maintab#init in
object(self)
  val mutable tabs = [ maintab ]
  val mutable seltab = maintab

  method draw =
    ui_checksize win_toosmall win_rows win_cols;
    if not !win_toosmall then begin
      ui_global tabs seltab;
      seltab#draw
    end;
    ui_refresh ()

  method handleInput = function
    | Key 0o632   -> (* screen resize *)
        true
    | Ctrl '\x03' -> (* ctrl+c *)
        do_quit := true; false
    | x           -> (* not a global key, let tab handle it *)
        seltab#handleInput x

end

