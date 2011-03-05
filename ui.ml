
type input_t = Key of int | Ctrl of char | Esc of string | Char of string


(* useful globals *)
let win_toosmall = ref false
let win_rows = ref 0
let win_cols = ref 0


class virtual tab = object
  method virtual getTitle : string
  method virtual getName : string
  method virtual draw : unit (* must draw between 0 < y < wincols-2 *)
  method virtual handleInput : input_t -> bool
  method setGlobal (gl : Commands.global) = ()
  method close = true
end


external ui_getinput : unit -> input_t list = "ui_getinput"
external ui_init : unit -> unit = "ui_init"
external ui_end : unit -> unit = "ui_end"
external ui_refresh : unit -> unit = "ui_refresh"
external ui_textinput_get : string -> string = "ui_textinput_get"
external ui_textinput_set : string -> string ref -> int ref -> unit = "ui_textinput_set"
external ui_textinput_key : input_t -> string ref -> int ref -> bool = "ui_textinput_key"
external ui_textinput_draw : (int * int * int) -> string -> int -> unit = "ui_textinput_draw"
external ui_logwindow_draw : (int * int * int * int) -> (float * string) array -> int -> unit = "ui_logwindow_draw"
external ui_checksize : bool ref -> int ref -> int ref -> unit = "ui_checksize"
external ui_global : tab list -> tab -> unit = "ui_global"
external ui_tab_main : unit -> unit = "ui_tab_main"



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



(* Scrolled logging "widget". Logs and displays a string with a timestamp. *)
class logWindow logfile =
  let reserved = 250 in (* number of empty buffer items, should be larger than win_cols *)
  let backlog = 2047 in (* must be 2^x-1 and larger than reserved+win_cols *)
object(self)
  val log = Array.make (backlog+1) (0.0, "") (* circular buffer *)
  val lf =
    if String.length logfile > 0 then Some (new Global.logfile logfile) else None
  val mutable lastlog = 0
  val mutable lastvisible = 0

  method write str =
    if lastlog = lastvisible then lastvisible <- lastlog + 1;
    lastlog <- lastlog + 1;
    log.(lastlog land backlog) <- (Unix.time (), str);
    log.((lastlog + reserved) land backlog) <- (0.0, "");
    match lf with None -> () | Some l -> l#write str

  method draw y x r c = ui_logwindow_draw (y,x,r,c) log lastvisible

  method scroll i =
    lastvisible <- min (lastvisible + i) lastlog;
    lastvisible <- max (lastlog - backlog + reserved) (max 1 lastvisible)
end



class hub name = object(self)
  inherit tab
  inherit Commands.hub
  val log = new logWindow ""
  val input = new textInput
  val cmd = new Commands.commands

  method getTitle = name^": Not connected"
  method getName = "#"^name
  method getHubName = name

  method private drawCmd = input#draw (!win_rows-3) 0 !win_cols
  method cmdReply str = log#write str

  (* TODO *)
  method draw =
    log#draw 1 0 (!win_rows-4) !win_cols;
    self#drawCmd

  method handleInput = function
      (* page down / up *)
    | Key 0o522 -> log#scroll (!win_rows / 2); true
    | Key 0o523 -> log#scroll (!win_rows / -2); true
      (* return *)
    | Ctrl '\n' ->
        let str = input#getText in
        if str <> "" then cmd#handle input#getText;
        input#setText "";
        true
      (* text input *)
    | k ->
        if input#handleInput k then self#drawCmd;
        false

  method setGlobal gl =
    cmd#setOrigin gl (Commands.Hub  (self :> Commands.hub))
end



class main = object(self)
  inherit tab
  inherit Commands.main

  val log = new logWindow "main"
  val cmd = new Commands.commands
  val input = new textInput;

  method getTitle = "Welcome to NCDC 0.1-alpha!"
  method getName = "main"

  method private drawCmd = input#draw (!win_rows-3) 9 (!win_cols-9)
  method cmdReply str = log#write str

  method draw =
    ui_tab_main ();
    log#draw 1 0 (!win_rows-4) !win_cols;
    self#drawCmd (* must be last to have the cursor at the correct position *)

  method handleInput = function
      (* page down / up *)
    | Key 0o522 -> log#scroll (!win_rows / 2); true
    | Key 0o523 -> log#scroll (!win_rows / -2); true
      (* return *)
    | Ctrl '\n' ->
        let str = input#getText in
        if str <> "" then cmd#handle input#getText;
        input#setText "";
        true
      (* text input *)
    | k ->
        if input#handleInput k then self#drawCmd;
        false

  method setGlobal gl =
    cmd#setOrigin gl (Commands.Main (self :> Commands.main))

  (* don't allow this tab to be closed *)
  method close = false

  initializer
    log#write "NCDC 0.1-alpha starting up...";
    log#write ("Using working directory: " ^ Global.workdir);

end



class global =
  let maintab = (new main :> tab) in
object(self)
  inherit Commands.global

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
      (* screen resize *)
    | Key 0o632   -> true
      (* ctrl+c *)
    | Ctrl '\x03' -> Global.do_quit := true; false
      (* Alt+[1-9]  (a bit ugly...) *)
    | Esc "1"     -> seltab <- List.nth tabs 0; true
    | Esc "2"     -> if List.length tabs > 1 then seltab <- List.nth tabs 1; true
    | Esc "3"     -> if List.length tabs > 2 then seltab <- List.nth tabs 2; true
    | Esc "4"     -> if List.length tabs > 3 then seltab <- List.nth tabs 3; true
    | Esc "5"     -> if List.length tabs > 4 then seltab <- List.nth tabs 4; true
    | Esc "6"     -> if List.length tabs > 5 then seltab <- List.nth tabs 5; true
    | Esc "7"     -> if List.length tabs > 6 then seltab <- List.nth tabs 6; true
    | Esc "8"     -> if List.length tabs > 7 then seltab <- List.nth tabs 7; true
    | Esc "9"     -> if List.length tabs > 8 then seltab <- List.nth tabs 8; true
    | Esc "0"     -> if List.length tabs > 9 then seltab <- List.nth tabs 9; true
      (* select next tab *)
    | Esc "k"     ->
        let rec n = function
          | [] | _ :: [] -> seltab
          | hd :: tl -> if hd == seltab then List.nth tl 0 else n tl
        in seltab <- n tabs; true
      (* select previous tab *)
    | Esc "j"     ->
        let rec p = function
          | [] | _ :: [] -> seltab
          | hd :: tl -> if List.nth tl 0 == seltab then hd else p tl
        in seltab <- p tabs; true
      (* move tab right *)
    | Esc "l"     ->
        let rec n = function
          | a :: (b :: c) -> if a == seltab then b :: (a :: c) else a :: n (b :: c)
          | a -> a
        in tabs <- n tabs; true
      (* move tab left *)
    | Esc "h"     ->
        let rec p = function
          | a :: (b :: c) -> if b == seltab then b :: (a :: c) else a :: p (b :: c)
          | a -> a
        in tabs <- p tabs; true
      (* close current tab *)
    | Esc "c"     -> self#cmdClose (Oo.id seltab); true
      (* not a global key, let tab handle it *)
    | x           -> seltab#handleInput x

  method cmdHubOpen n =
    try
      seltab <- List.find (fun t -> t#getName = "#"^n) tabs
    with Not_found ->
      seltab <- (new hub n :> tab);
      seltab#setGlobal (self :> Commands.global);
      tabs <- tabs @ [seltab]

  (* Accepts the Oo.id of the tab to close. The reason for this is that tabs can
   * be closed both from a command handler and from a keyboard shortcut. These
   * two locations have different views (types) on the tab objects, and we
   * cannot cast between them. *)
  method cmdClose t =
    tabs <- List.filter (fun n ->
      if Oo.id n = t then not n#close else true) tabs;
    if Oo.id seltab = t then seltab <- List.hd tabs

  initializer
    maintab#setGlobal (self :> Commands.global)

end

