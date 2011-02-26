

let ui = new Ui.global

let _ =
  let lastredraw = ref (Unix.time ()) in

  (* init curses *)
  Ui.ui_init ();
  ui#draw;

  (* mainloop *)
  while not !Ui.do_quit do
    (* select *)
    let _ =
      try
        Unix.select [Unix.stdin] [] [] 0.1
      with Unix.Unix_error (Unix.EINTR,_,_) -> ([],[],[])
    in
    (* process input from terminal *)
    Ui.do_redraw := max !Ui.do_redraw (List.exists (fun b->b)
      (List.map ui#handleInput (Ui.ui_getinput ()))
    );
    (* make sure to redraw the screen every second *)
    if !lastredraw < (Unix.time () -. 1.0) then (
      Ui.do_redraw := true;
      lastredraw := Unix.time ()
    );
    if !Ui.do_redraw then (
      Ui.do_redraw := false;
      ui#draw
    )
  done;

  Ui.ui_end ()

