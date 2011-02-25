

let ui = new Ui.global

let _ =
  (* init curses *)
  Ui.ui_init ();
  ui#draw;

  (* mainloop *)
  while not !Ui.do_quit do
    (* select *)
    let _ =
      try
        Unix.select [Unix.stdin] [] [] (-1.0)
      with Unix.Unix_error (Unix.EINTR,_,_) -> ([],[],[])
    in
    (* let the Ui modules handle input and redraw when requested *)
    if List.exists (fun b->b) (List.map ui#handleInput (Ui.ui_getinput ())) then
      ui#draw;
  done;

  Ui.ui_end ()

