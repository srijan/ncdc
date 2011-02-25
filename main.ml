

let ui = new Ui.global

let _ =
  (* init curses *)
  Ui.ui_init ();

  (* draw main display *)
  ui#draw;
  Unix.sleep 5;

  Ui.ui_end ()

