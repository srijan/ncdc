
type input_t = Key of int | Ctrl of char | Esc of string | Char of string


(* global variables to signal the mainloop to do something *)
let do_redraw = ref true
let do_quit = ref false


external ui_getinput : unit -> input_t list = "ui_getinput"
external ui_init : unit -> unit = "ui_init"
external ui_end : unit -> unit = "ui_end"
external ui_global : string -> (string * bool) list -> bool = "ui_global"
external ui_tab_main : string array -> int -> int -> unit = "ui_tab_main"


class virtual tab = object
  method virtual getTitle : string
  method virtual getName : string
  method virtual draw : unit (* must draw between 0 < y < wincols-2 *)
end



class main =
  let backlog = 511 in (* must be 2^x-1 *)
object(self)
  inherit tab
  val log = Array.make (backlog+1) "" (* circular buffer *)
  val lastlog = ref 0

  method getTitle = "Welcome to NCDC 0.1-alpha!"
  method getName = "main"

  method private addline str =
    lastlog := (!lastlog + 1) land backlog;
    log.(!lastlog) <- str

  method draw =
    for i = 1 to 1000 do
      self#addline ("Line " ^ (string_of_int i))
    done;
    self#addline ("This is a very long line indeed. " ^ 
      "This is a very long line indeed. "^"This is a very long line indeed. "^ 
      "This is é very long おたくの娘さん "^"This is a very long line indeed.");
    ui_tab_main log !lastlog 1

end



class global =
  let maintab = new main in
object(self)
  val mutable tabs = ([ maintab ] : tab list)
  val mutable seltab = (maintab : tab)

  method draw =
    do_redraw := false;
    (* convert tab list into something easy to work with in C *)
    let t = List.fold_left (fun p tab ->
      (tab#getName, tab = seltab) :: p) [] tabs in
    (* call c function and draw tab if the screen is large enough *)
    if not (ui_global seltab#getTitle t) then seltab#draw

  method handleInput t = match t with
    | Key 0o632 -> do_redraw := true (* screen resize *)
    | Ctrl '\x03' -> do_quit := true (* ctrl+c *)
    | _ -> ()

end

