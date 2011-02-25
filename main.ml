

let ui = new Ui.global

let _ =
  (* init curses *)
  Ui.ui_init ();

  (* mainloop *)
  while not !Ui.do_quit do
    if !Ui.do_redraw then ui#draw;
    let _ =
      try
        Unix.select [Unix.stdin] [] [] (-1.0)
      with Unix.Unix_error (Unix.EINTR,_,_) -> ([],[],[])
    in
    List.iter ui#handleInput (Ui.ui_getinput ());
  done;

  Ui.ui_end ()

