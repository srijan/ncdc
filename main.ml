

let ui = new Ui.global

let _ =
  let lastredraw = ref (Unix.time ()) in

  (* init curses *)
  Ui.ui_init ();
  ui#draw;

  (* mainloop *)
  while not !Global.do_quit do
    (* select *)
    let _ =
      try
        Unix.select [Unix.stdin] [] [] 0.1
      with Unix.Unix_error (Unix.EINTR,_,_) -> ([],[],[])
    in
    (* process input from terminal *)
    Global.do_redraw := max !Global.do_redraw (List.exists (fun b->b)
      (List.map ui#handleInput (Ui.ui_getinput ()))
    );
    (* make sure to redraw the screen every second *)
    if !lastredraw < (Unix.time () -. 1.0) then (
      Global.do_redraw := true;
      lastredraw := Unix.time ()
    );
    if !Global.do_redraw then (
      Global.do_redraw := false;
      ui#draw
    )
  done;

  Ui.ui_end ();
  Global.Conf.close ()

