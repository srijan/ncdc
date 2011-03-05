

let ui = new Ui.global

let _ =
  let lastredraw = ref (Unix.time ()) in

  (* init curses *)
  Ui.ui_init ();
  ui#draw;

  (* create fd hashtables *)
  let rd = Hashtbl.create 10 in
  let wr = Hashtbl.create 10 in

  (* mainloop *)
  while not !Global.do_quit do
    (* fill fd hashtables *)
    Hashtbl.clear rd;
    Hashtbl.clear wr;
    Hashtbl.add rd Unix.stdin (fun () -> ());
    Hashtbl.iter (fun _ h -> h#setSelectMasks rd wr) Global.hubs;
    let rdf = Hashtbl.fold (fun a _ b -> a :: b) rd [] in
    let wrf = Hashtbl.fold (fun a _ b -> a :: b) wr [] in
    (* select *)
    let canrd, canwr, _ =
      try Unix.select rdf wrf [] 0.1
      with Unix.Unix_error (Unix.EINTR,_,_) -> ([],[],[])
    in
    (* process input from terminal *)
    Global.do_redraw := max !Global.do_redraw (List.exists (fun b->b)
      (List.map ui#handleInput (Ui.ui_getinput ()))
    );
    (* process I/O *)
    List.iter (fun s -> (Hashtbl.find rd s) ()) canrd;
    List.iter (fun s -> (Hashtbl.find wr s) ()) canwr;
    (* make sure to redraw the screen every second *)
    if !lastredraw < (Unix.time () -. 1.0) then (
      Global.do_redraw := true;
      lastredraw := Unix.time ()
    );
    (* redraw screen when required *)
    if !Global.do_redraw then (
      Global.do_redraw := false;
      ui#draw
    )
  done;

  Ui.ui_end ();
  Global.Conf.close ()

