#include "shod.h"

#define DIV                     15      /* see containerplace() for details */

/* get next focused container after old on given monitor and desktop */
struct Container *
getnextfocused(struct Monitor *mon, int desk)
{
	struct Container *c;

	TAILQ_FOREACH(c, &wm.focusq, entry)
		if (containerisvisible(c, mon, desk))
			return c;
	return NULL;
}

/* snap to edge */
static void
snaptoedge(int *x, int *y, int w, int h)
{
	struct Container *c;

	if (config.snap <= 0)
		return;
	if (abs(*y - wm.selmon->wy) < config.snap)
		*y = wm.selmon->wy;
	if (abs(*y + h - wm.selmon->wy - wm.selmon->wh) < config.snap)
		*y = wm.selmon->wy + wm.selmon->wh - h;
	if (abs(*x - wm.selmon->wx) < config.snap)
		*x = wm.selmon->wx;
	if (abs(*x + w - wm.selmon->wx - wm.selmon->ww) < config.snap)
		*x = wm.selmon->wx + wm.selmon->ww - w;
	TAILQ_FOREACH(c, &wm.focusq, entry) {
		if (!c->isminimized && containerisvisible(c, wm.selmon, wm.selmon->seldesk)) {
			if (*x + w >= c->x && *x <= c->x + c->w) {
				if (abs(*y + h - c->y) < config.snap) {
					*y = c->y - h;
				}
				if (abs(*y - c->y) < config.snap) {
					*y = c->y;
				}
				if (abs(*y + h - c->y - c->h) < config.snap) {
					*y = c->y + c->h - h;
				}
				if (abs(*y - c->y - c->h) < config.snap) {
					*y = c->y + c->h;
				}
			}
			if (*y + h >= c->y && *y <= c->y + c->h) {
				if (abs(*x + w - c->x) < config.snap) {
					*x = c->x - w;
				}
				if (abs(*x - c->x) < config.snap) {
					*x = c->x;
				}
				if (abs(*x + w - c->x - c->w) < config.snap) {
					*x = c->x + c->w - w;
				}
				if (abs(*x - c->x - c->w) < config.snap) {
					*x = c->x + c->w;
				}
			}
		}
	}
}

/* increment number of clients */
static void
clientsincr(void)
{
	wm.nclients++;
}

/* decrement number of clients */
static void
clientsdecr(void)
{
	wm.nclients--;
}

/* get decoration style (and state) of container */
static int
containergetstyle(struct Container *c)
{
	return (c == wm.focused) ? FOCUSED : UNFOCUSED;
}

/* get tab decoration style */
static int
tabgetstyle(struct Tab *t)
{
	if (t == NULL)
		return UNFOCUSED;
	if (t->isurgent)
		return URGENT;
	if (t->row->col->c == wm.focused)
		return FOCUSED;
	return UNFOCUSED;
}

/* clear window urgency */
static void
tabclearurgency(struct Tab *tab)
{
	XWMHints wmh = {0};

	XSetWMHints(dpy, tab->obj.win, &wmh);
	tab->isurgent = 0;
}

/* commit tab size and position */
static void
tabmoveresize(struct Tab *t)
{
	XMoveResizeWindow(dpy, t->title, t->x, 0, t->w, config.titlewidth);
	if (t->ptw != t->w) {
		tabdecorate(t, 0);
	}
	winnotify(t->obj.win, t->row->col->c->x + t->row->col->x, t->row->col->c->y + t->row->y + config.titlewidth, t->winw, t->winh);
}

/* commit titlebar size and position */
static void
titlebarmoveresize(struct Row *row, int x, int y, int w)
{
	XMoveResizeWindow(dpy, row->bar, x, y, w, config.titlewidth);
	XMoveWindow(dpy, row->bl, 0, 0);
	XMoveWindow(dpy, row->br, w - config.titlewidth, 0);
}

/* calculate size of dialogs of a tab */
static void
dialogcalcsize(struct Dialog *dial)
{
	struct Tab *tab;

	tab = dial->tab;
	dial->w = max(1, min(dial->maxw, tab->winw - 2 * config.borderwidth));
	dial->h = max(1, min(dial->maxh, tab->winh - 2 * config.borderwidth));
	dial->x = tab->winw / 2 - dial->w / 2;
	dial->y = tab->winh / 2 - dial->h / 2;
}

/* create new dialog */
static struct Dialog *
dialognew(Window win, int maxw, int maxh, int ignoreunmap)
{
	struct Dialog *dial;

	dial = emalloc(sizeof(*dial));
	*dial = (struct Dialog){
		.pix = None,
		.maxw = maxw,
		.maxh = maxh,
		.ignoreunmap = ignoreunmap,
		.obj.win = win,
		.obj.type = TYPE_DIALOG,
	};
	dial->frame = XCreateWindow(dpy, root, 0, 0, maxw, maxh, 0, depth, CopyFromParent, visual, clientmask, &clientswa);
	XReparentWindow(dpy, dial->obj.win, dial->frame, 0, 0);
	XMapWindow(dpy, dial->obj.win);
	return dial;
}

/* calculate position and width of tabs of a row */
static void
rowcalctabs(struct Row *row)
{
	struct Object *p, *q;
	struct Dialog *d;
	struct Tab *t;
	int i, x;

	if (TAILQ_EMPTY(&row->tabq))
		return;
	x = config.titlewidth;
	i = 0;
	TAILQ_FOREACH(p, &row->tabq, entry) {
		t = (struct Tab *)p;
		t->winh = max(1, row->h - config.titlewidth);
		t->winw = row->col->w;
		t->w = max(1, ((i + 1) * (t->winw - 2 * config.titlewidth) / row->ntabs) - (i * (t->winw - 2 * config.titlewidth) / row->ntabs));
		t->x = x;
		x += t->w;
		TAILQ_FOREACH(q, &t->dialq, entry) {
			d = (struct Dialog *)q;
			dialogcalcsize(d);
		}
		i++;
	}
}

/* calculate position and height of rows of a column */
static void
colcalcrows(struct Column *col, int recalcfact)
{
	struct Container *c;
	struct Row *row;
	int i, y, h, sumh;
	int content;
	int recalc;

	c = col->c;

	if (TAILQ_EMPTY(&col->rowq))
		return;
	if (col->c->isfullscreen) {
		TAILQ_FOREACH(row, &col->rowq, entry) {
			row->y = -config.titlewidth;
			row->h = col->c->h + config.titlewidth;
			rowcalctabs(row);
		}
		return;
	}

	/* check if rows sum up the height of the container */
	content = columncontentheight(col);
	sumh = 0;
	recalc = 0;
	TAILQ_FOREACH(row, &col->rowq, entry) {
		if (!recalcfact) {
			if (TAILQ_NEXT(row, entry) == NULL) {
				row->h = content - sumh + config.titlewidth;
			} else {
				row->h = row->fact * content + config.titlewidth;
			}
			if (row->h < config.titlewidth) {
				recalc = 1;
			}
		}
		sumh += row->h - config.titlewidth;
	}
	if (sumh != content)
		recalc = 1;

	h = col->c->h - 2 * c->b - (col->nrows - 1) * config.divwidth;
	y = c->b;
	i = 0;
	TAILQ_FOREACH(row, &col->rowq, entry) {
		if (recalc)
			row->h = max(config.titlewidth, ((i + 1) * h / col->nrows) - (i * h / col->nrows));
		if (recalc || recalcfact)
			row->fact = (double)(row->h - config.titlewidth) / (double)(content);
		row->y = y;
		y += row->h + config.divwidth;
		rowcalctabs(row);
		i++;
	}
}

/* create new tab */
static struct Tab *
tabnew(Window win, Window leader, int ignoreunmap)
{
	struct Tab *tab;

	tab = emalloc(sizeof(*tab));
	*tab = (struct Tab){
		.ignoreunmap = ignoreunmap,
		.pix = None,
		.pixtitle = None,
		.title = None,
		.leader = leader,
		.obj.win = win,
		.obj.type = TYPE_NORMAL,
	};
	TAILQ_INIT(&tab->dialq);
	((struct Object *)tab)->type = TYPE_NORMAL;
	tab->frame = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, depth, CopyFromParent, visual, clientmask, &clientswa),
	XReparentWindow(dpy, tab->obj.win, tab->frame, 0, 0);
	XMapWindow(dpy, tab->obj.win);
	icccmwmstate(win, NormalState);
	clientsincr();
	return tab;
}

/* remove tab from row's tab queue */
static void
tabremove(struct Row *row, struct Tab *tab)
{
	if (row->seltab == tab) {
		row->seltab = (struct Tab *)TAILQ_PREV((struct Object *)tab, Queue, entry);
		if (row->seltab == NULL) {
			row->seltab = (struct Tab *)TAILQ_NEXT((struct Object *)tab, entry);
		}
	}
	row->ntabs--;
	TAILQ_REMOVE(&row->tabq, (struct Object *)tab, entry);
	tab->row = NULL;
}

/* delete tab */
static void
tabdel(struct Tab *tab)
{
	struct Dialog *dial;

	while ((dial = (struct Dialog *)TAILQ_FIRST(&tab->dialq)) != NULL) {
		XDestroyWindow(dpy, dial->obj.win);
		unmanagedialog((struct Object *)dial, 0);
	}
	tabremove(tab->row, tab);
	if (tab->pixtitle != None)
		XFreePixmap(dpy, tab->pixtitle);
	if (tab->pix != None)
		XFreePixmap(dpy, tab->pix);
	icccmdeletestate(tab->obj.win);
	XReparentWindow(dpy, tab->obj.win, root, 0, 0);
	XDestroyWindow(dpy, tab->title);
	XDestroyWindow(dpy, tab->frame);
	clientsdecr();
	free(tab->name);
	free(tab);
}

/* create new row */
struct Row *
rownew(void)
{
	struct Row *row;

	row = emalloc(sizeof(*row));
	*row = (struct Row){
		.pixbar = None,
		.isunmapped = 0,
	};
	TAILQ_INIT(&row->tabq);
	row->frame = XCreateWindow(dpy, root, 0, 0, 1, 1, 0,
	                           depth, CopyFromParent, visual,
	                           clientmask, &clientswa);
	row->bar = XCreateWindow(dpy, root, 0, 0, 1, 1, 0,
	                         depth, CopyFromParent, visual,
	                         clientmask, &clientswa);
	row->bl = XCreateWindow(dpy, row->bar, 0, 0, config.titlewidth, config.titlewidth, 0,
	                        depth, CopyFromParent, visual,
	                        clientmask, &clientswa);
	row->pixbl = XCreatePixmap(dpy, row->bl, config.titlewidth, config.titlewidth, depth);
	row->br = XCreateWindow(dpy, row->bar, 0, 0, config.titlewidth, config.titlewidth, 0,
	                        depth, CopyFromParent, visual,
	                        clientmask, &clientswa);
	row->div = XCreateWindow(dpy, root, 0, 0, 1, 1, 0,
	                         CopyFromParent, InputOnly, CopyFromParent, CWCursor,
	                         &(XSetWindowAttributes){.cursor = wm.cursors[CURSOR_V]});
	row->pixbr = XCreatePixmap(dpy, row->bl, config.titlewidth, config.titlewidth, depth);
	XMapWindow(dpy, row->bl);
	XMapWindow(dpy, row->br);
	XDefineCursor(dpy, row->bl, wm.cursors[CURSOR_HAND]);
	XDefineCursor(dpy, row->br, wm.cursors[CURSOR_PIRATE]);
	return row;
}

/* detach row from column */
static void
rowdetach(struct Row *row, int recalc)
{
	struct Column *col;

	col = row->col;
	if (col->selrow == row) {
		col->selrow = TAILQ_PREV(row, RowQueue, entry);
		if (col->selrow == NULL) {
			col->selrow = TAILQ_NEXT(row, entry);
		}
	}
	col->nrows--;
	TAILQ_REMOVE(&col->rowq, row, entry);
	if (recalc) {
		colcalcrows(row->col, 1);
	}
}

/* delete row */
static void
rowdel(struct Row *row)
{
	struct Tab *tab;

	while ((tab = (struct Tab *)TAILQ_FIRST(&row->tabq)) != NULL)
		tabdel(tab);
	rowdetach(row, 1);
	XDestroyWindow(dpy, row->frame);
	XDestroyWindow(dpy, row->bar);
	XDestroyWindow(dpy, row->bl);
	XDestroyWindow(dpy, row->br);
	XDestroyWindow(dpy, row->div);
	if (row->pixbar != None)
		XFreePixmap(dpy, row->pixbar);
	XFreePixmap(dpy, row->pixbl);
	XFreePixmap(dpy, row->pixbr);
	free(row);
}

/* create new column */
static struct Column *
colnew(void)
{
	struct Column *col;

	col = emalloc(sizeof(*col));
	*col = (struct Column){ 0 };
	TAILQ_INIT(&col->rowq);
	col->div = XCreateWindow(dpy, root, 0, 0, 1, 1, 0,
	                         CopyFromParent, InputOnly, CopyFromParent, CWCursor,
	                         &(XSetWindowAttributes){.cursor = wm.cursors[CURSOR_H]});
	return col;
}

/* detach column from container */
static void
coldetach(struct Column *col)
{
	struct Container *c;

	c = col->c;
	if (c->selcol == col) {
		c->selcol = TAILQ_PREV(col, ColumnQueue, entry);
		if (c->selcol == NULL) {
			c->selcol = TAILQ_NEXT(col, entry);
		}
	}
	c->ncols--;
	TAILQ_REMOVE(&c->colq, col, entry);
	containercalccols(col->c);
}

/* delete column */
static void
coldel(struct Column *col)
{
	struct Row *row;

	while ((row = TAILQ_FIRST(&col->rowq)) != NULL)
		rowdel(row);
	coldetach(col);
	XDestroyWindow(dpy, col->div);
	free(col);
}

/* add row to column */
static void
coladdrow(struct Column *col, struct Row *row, struct Row *prev)
{
	struct Container *c;
	struct Column *oldcol;

	c = col->c;
	oldcol = row->col;
	row->col = col;
	col->selrow = row;
	col->nrows++;
	if (prev == NULL || TAILQ_EMPTY(&col->rowq))
		TAILQ_INSERT_HEAD(&col->rowq, row, entry);
	else
		TAILQ_INSERT_AFTER(&col->rowq, prev, row, entry);
	colcalcrows(col, 1);    /* set row->y, row->h, etc */
	XReparentWindow(dpy, row->div, c->frame, col->x + col->w, c->b);
	XReparentWindow(dpy, row->bar, c->frame, col->x, row->y);
	XReparentWindow(dpy, row->frame, c->frame, col->x, row->y);
	XMapWindow(dpy, row->bar);
	XMapWindow(dpy, row->frame);
	if (oldcol != NULL && oldcol->nrows == 0) {
		coldel(oldcol);
	}
}

/* add tab to row */
static void
rowaddtab(struct Row *row, struct Tab *tab, struct Tab *prev)
{
	struct Row *oldrow;

	oldrow = tab->row;
	tab->row = row;
	row->seltab = tab;
	row->ntabs++;
	if (prev == NULL || TAILQ_EMPTY(&row->tabq))
		TAILQ_INSERT_HEAD(&row->tabq, (struct Object *)tab, entry);
	else
		TAILQ_INSERT_AFTER(&row->tabq, (struct Object *)prev, (struct Object *)tab, entry);
	rowcalctabs(row);               /* set tab->x, tab->w, etc */
	if (tab->title == None) {
		tab->title = XCreateWindow(dpy, row->bar, tab->x, 0, tab->w, config.titlewidth, 0,
		                         depth, CopyFromParent, visual,
		                         clientmask, &clientswa);
	} else {
		XReparentWindow(dpy, tab->title, row->bar, tab->x, 0);
	}
	XReparentWindow(dpy, tab->frame, row->frame, 0, 0);
	XMapWindow(dpy, tab->frame);
	XMapWindow(dpy, tab->title);
	if (oldrow != NULL) {           /* deal with the row this tab came from */
		if (oldrow->ntabs == 0) {
			rowdel(oldrow);
		} else {
			rowcalctabs(oldrow);
		}
	}
}

/* decorate dialog window */
static void
dialogdecorate(struct Dialog *d)
{
	int fullw, fullh;       /* size of dialog window + borders */

	fullw = d->w + 2 * config.borderwidth;
	fullh = d->h + 2 * config.borderwidth;

	/* (re)create pixmap */
	if (d->pw != fullw || d->ph != fullh || d->pix == None)
		pixmapnew(&d->pix, d->frame, fullw, fullh);
	d->pw = fullw;
	d->ph = fullh;

	drawborders(d->pix, fullw, fullh, tabgetstyle(d->tab));
	drawcommit(d->pix, d->frame);
}

/* get focused fullscreen window in given monitor and desktop */
static struct Container *
getfullscreen(struct Monitor *mon, int desk)
{
	struct Container *c;

	for (c = &wm.layers[LAYER_FULLSCREEN]; !ISDUMMY(c); c = TAILQ_NEXT(c, raiseentry))
		if (containerisvisible(c, mon, desk))
			return c;
	return NULL;
}

/* add container into head of focus queue */
static void
containerinsertfocus(struct Container *c)
{
	TAILQ_INSERT_HEAD(&wm.focusq, c, entry);
}

/* add container into head of focus queue */
static void
containerinsertraise(struct Container *c)
{
	int layer;

	layer = LAYER_NORMAL;
	if (c->isfullscreen)
		layer = LAYER_FULLSCREEN;
	else if (c->abovebelow > 0)
		layer = LAYER_ABOVE;
	else if (c->abovebelow < 0)
		layer = LAYER_BELOW;
	TAILQ_INSERT_AFTER(&wm.stackq, &wm.layers[layer], c, raiseentry);
}

/* remove container from the focus list */
static void
containerdelfocus(struct Container *c)
{
	TAILQ_REMOVE(&wm.focusq, c, entry);
}

/* put container on beginning of focus list */
static void
containeraddfocus(struct Container *c)
{
	if (c == NULL || c->isminimized)
		return;
	containerdelfocus(c);
	containerinsertfocus(c);
}

/* remove container from the raise list */
static void
containerdelraise(struct Container *c)
{
	TAILQ_REMOVE(&wm.stackq, c, raiseentry);
}

/* hide container */
void
containerhide(struct Container *c, int hide)
{
	struct Object *t, *d;

	if (c == NULL)
		return;
	c->ishidden = hide;
	if (hide) {
		XUnmapWindow(dpy, c->frame);
	} else {
		XMapWindow(dpy, c->frame);
	}
	TAB_FOREACH_BEGIN(c, t) {
		icccmwmstate(t->win, (hide ? IconicState : NormalState));
		TAILQ_FOREACH(d, &((struct Tab *)t)->dialq, entry) {
			icccmwmstate(d->win, (hide ? IconicState : NormalState));
		}
	}TAB_FOREACH_END
}

/* add column to container */
static void
containeraddcol(struct Container *c, struct Column *col, struct Column *prev)
{
	struct Container *oldc;

	oldc = col->c;
	col->c = c;
	c->selcol = col;
	c->ncols++;
	if (prev == NULL || TAILQ_EMPTY(&c->colq))
		TAILQ_INSERT_HEAD(&c->colq, col, entry);
	else
		TAILQ_INSERT_AFTER(&c->colq, prev, col, entry);
	XReparentWindow(dpy, col->div, c->frame, 0, 0);
	containercalccols(c);
	if (oldc != NULL && oldc->ncols == 0) {
		containerdel(oldc);
	}
}

/* send container to desktop and raise it; return nonzero if it was actually sent anywhere */
static int
containersendtodesk(struct Container *c, struct Monitor *mon, unsigned long desk)
{
	void containerstick(struct Container *c, int stick);

	if (c == NULL || c->isminimized)
		return 0;
	if (desk == 0xFFFFFFFF) {
		containerstick(c, ADD);
	} else if ((int)desk < config.ndesktops) {
		c->desk = (int)desk;
		if (c->mon != mon)
			containerplace(c, mon, desk, 1);
		c->mon = mon;
		c->issticky = 0;
		if ((int)desk != mon->seldesk)  /* container was sent to invisible desktop */
			containerhide(c, 1);
		else
			containerhide(c, 0);
		containerraise(c, c->isfullscreen, c->abovebelow);
	} else {
		return 0;
	}
	ewmhsetwmdesktop(c);
	ewmhsetstate(c);
	return 1;
}

/* make a container occupy the whole monitor */
static void
containerfullscreen(struct Container *c, int fullscreen)
{
	if (fullscreen != REMOVE && !c->isfullscreen)
		containerraise(c, 1, c->abovebelow);
	else if (fullscreen != ADD && c->isfullscreen)
		containerraise(c, 0, c->abovebelow);
	else
		return;
	containercalccols(c);
	containermoveresize(c, 1);
	containerredecorate(c, NULL, NULL, 0);
	ewmhsetstate(c);
}

/* maximize a container on the monitor */
static void
containermaximize(struct Container *c, int maximize)
{
	if (maximize != REMOVE && !c->ismaximized)
		c->ismaximized = 1;
	else if (maximize != ADD && c->ismaximized)
		c->ismaximized = 0;
	else
		return;
	containercalccols(c);
	containermoveresize(c, 1);
	containerredecorate(c, NULL, NULL, 0);
}

/* minimize container; optionally focus another container */
static void
containerminimize(struct Container *c, int minimize, int focus)
{
	struct Container *tofocus;

	if (minimize != REMOVE && !c->isminimized) {
		c->isminimized = 1;
		containerhide(c, 1);
		if (focus) {
			if ((tofocus = getnextfocused(c->mon, c->desk)) != NULL) {
				tabfocus(tofocus->selcol->selrow->seltab, 0);
				containerraise(c, c->isfullscreen, c->abovebelow);
			} else {
				tabfocus(NULL, 0);
			}
		}
	} else if (minimize != ADD && c->isminimized) {
		(void)containersendtodesk(c, wm.selmon, wm.selmon->seldesk);
		containermoveresize(c, 1);
		tabfocus(c->selcol->selrow->seltab, 0);
		containerraise(c, c->isfullscreen, c->abovebelow);
	}
}

/* shade container title bar */
static void
containershade(struct Container *c, int shade)
{
	if (shade != REMOVE && !c->isshaded) {
		c->isshaded = 1;
		XDefineCursor(dpy, c->curswin[BORDER_NW], wm.cursors[CURSOR_W]);
		XDefineCursor(dpy, c->curswin[BORDER_SW], wm.cursors[CURSOR_W]);
		XDefineCursor(dpy, c->curswin[BORDER_NE], wm.cursors[CURSOR_E]);
		XDefineCursor(dpy, c->curswin[BORDER_SE], wm.cursors[CURSOR_E]);
	} else if (shade != ADD && c->isshaded) {
		c->isshaded = 0;
		XDefineCursor(dpy, c->curswin[BORDER_NW], wm.cursors[CURSOR_NW]);
		XDefineCursor(dpy, c->curswin[BORDER_SW], wm.cursors[CURSOR_SW]);
		XDefineCursor(dpy, c->curswin[BORDER_NE], wm.cursors[CURSOR_NE]);
		XDefineCursor(dpy, c->curswin[BORDER_SE], wm.cursors[CURSOR_SE]);
	} else {
		return;
	}
	containercalccols(c);
	containermoveresize(c, 1);
	containerredecorate(c, NULL, NULL, 0);
	if (c == wm.focused) {
		tabfocus(c->selcol->selrow->seltab, 0);
	}
}

/* stick a container on the monitor */
void
containerstick(struct Container *c, int stick)
{
	if (stick != REMOVE && !c->issticky) {
		c->issticky = 1;
		ewmhsetwmdesktop(c);
	} else if (stick != ADD && c->issticky) {
		c->issticky = 0;
		(void)containersendtodesk(c, c->mon, c->mon->seldesk);
	} else {
		return;
	}
}

/* raise container above others */
static void
containerabove(struct Container *c, int above)
{
	if (above != REMOVE && c->abovebelow != 1)
		containerraise(c, c->isfullscreen, 1);
	else if (above != ADD && c->abovebelow != 0)
		containerraise(c, c->isfullscreen, 0);
	else
		return;
}

/* lower container below others */
static void
containerbelow(struct Container *c, int below)
{
	if (below != REMOVE && c->abovebelow != -1)
		containerraise(c, c->isfullscreen, -1);
	else if (below != ADD && c->abovebelow != 0)
		containerraise(c, c->isfullscreen, 0);
	else
		return;
}

/* create new container */
struct Container *
containernew(int x, int y, int w, int h, int state)
{
	struct Container *c;
	int i;

	x -= config.borderwidth,
	y -= config.borderwidth,
	w += 2 * config.borderwidth,
	h += 2 * config.borderwidth + config.titlewidth,
	c = emalloc(sizeof *c);
	*c = (struct Container) {
		.x  = x, .y  = y, .w  = w, .h  = h,
		.nx = x, .ny = y, .nw = w, .nh = h,
		.b = config.borderwidth,
		.pix = None,
		.isfullscreen = (state & FULLSCREEN),
		.ismaximized = (state & MAXIMIZED),
		.isminimized = (state & MINIMIZED),
		.issticky = (state & STICKY),
		.isshaded = (state & SHADED),
		.ishidden = 0,
		.isobscured = 0,
		.abovebelow = (state & ABOVE) ? +1 : (state & BELOW) ? -1 : 0,
	};
	TAILQ_INIT(&c->colq);
	c->frame = XCreateWindow(dpy, root, c->x, c->y, c->w, c->h, 0, depth, InputOutput, visual, clientmask, &clientswa);
	c->curswin[BORDER_N] = XCreateWindow(
		dpy, c->frame, 0, 0, 1, 1, 0,
		CopyFromParent, InputOnly, CopyFromParent,
		CWCursor,
		&(XSetWindowAttributes){
			.cursor = wm.cursors[CURSOR_N],
		}
	);
	c->curswin[BORDER_S] = XCreateWindow(
		dpy, c->frame, 0, 0, 1, 1, 0,
		CopyFromParent, InputOnly, CopyFromParent,
		CWCursor,
		&(XSetWindowAttributes){
			.cursor = wm.cursors[CURSOR_S],
		}
	);
	c->curswin[BORDER_W] = XCreateWindow(
		dpy, c->frame, 0, 0, 1, 1, 0,
		CopyFromParent, InputOnly, CopyFromParent,
		CWCursor,
		&(XSetWindowAttributes){
			.cursor = wm.cursors[CURSOR_W],
		}
	);
	c->curswin[BORDER_E] = XCreateWindow(
		dpy, c->frame, 0, 0, 1, 1, 0,
		CopyFromParent, InputOnly, CopyFromParent,
		CWCursor,
		&(XSetWindowAttributes){
			.cursor = wm.cursors[CURSOR_E],
		}
	);
	c->curswin[BORDER_NW] = XCreateWindow(
		dpy, c->frame, 0, 0, 1, 1, 0,
		CopyFromParent, InputOnly, CopyFromParent,
		CWCursor,
		&(XSetWindowAttributes){
			.cursor = c->isshaded ? wm.cursors[CURSOR_W] : wm.cursors[CURSOR_NW],
		}
	);
	c->curswin[BORDER_SW] = XCreateWindow(
		dpy, c->frame, 0, 0, 1, 1, 0,
		CopyFromParent, InputOnly, CopyFromParent,
		CWCursor,
		&(XSetWindowAttributes){
			.cursor = c->isshaded ? wm.cursors[CURSOR_W] : wm.cursors[CURSOR_SW],
		}
	);
	c->curswin[BORDER_NE] = XCreateWindow(
		dpy, c->frame, 0, 0, 1, 1, 0,
		CopyFromParent, InputOnly, CopyFromParent,
		CWCursor,
		&(XSetWindowAttributes){
			.cursor = c->isshaded ? wm.cursors[CURSOR_E] : wm.cursors[CURSOR_NE],
		}
	);
	c->curswin[BORDER_SE] = XCreateWindow(
		dpy, c->frame, 0, 0, 1, 1, 0,
		CopyFromParent, InputOnly, CopyFromParent,
		CWCursor,
		&(XSetWindowAttributes){
			.cursor = c->isshaded ? wm.cursors[CURSOR_E] : wm.cursors[CURSOR_SE],
		}
	);
	for (i = 0; i < BORDER_LAST; i++)
		XMapWindow(dpy, c->curswin[i]);
	containerinsertfocus(c);
	containerinsertraise(c);
	return c;
}

/* delete container */
void
containerdel(struct Container *c)
{
	struct Column *col;
	int i;

	containerdelfocus(c);
	containerdelraise(c);
	if (wm.focused == c)
		wm.focused = NULL;
	TAILQ_REMOVE(&wm.focusq, c, entry);
	while ((col = TAILQ_FIRST(&c->colq)) != NULL)
		coldel(col);
	if (c->pix != None)
		XFreePixmap(dpy, c->pix);
	XDestroyWindow(dpy, c->frame);
	for (i = 0; i < BORDER_LAST; i++)
		XDestroyWindow(dpy, c->curswin[i]);
	free(c);
}

/* commit container size and position */
void
containermoveresize(struct Container *c, int checkstack)
{
	struct Object *t, *d;
	struct Column *col;
	struct Row *row;
	struct Tab *tab;
	struct Dialog *dial;
	int rowy;
	int isshaded;

	if (c == NULL)
		return;
	XMoveResizeWindow(dpy, c->frame, c->x, c->y, c->w, c->h);
	XMoveResizeWindow(dpy, c->curswin[BORDER_N], config.corner, 0, c->w - 2 * config.corner, c->b);
	XMoveResizeWindow(dpy, c->curswin[BORDER_S], config.corner, c->h - c->b, c->w - 2 * config.corner, c->b);
	XMoveResizeWindow(dpy, c->curswin[BORDER_W], 0, config.corner, c->b, c->h - 2 * config.corner);
	XMoveResizeWindow(dpy, c->curswin[BORDER_E], c->w - c->b, config.corner, c->b, c->h - 2 * config.corner);
	XMoveResizeWindow(dpy, c->curswin[BORDER_NW], 0, 0, config.corner, config.corner);
	XMoveResizeWindow(dpy, c->curswin[BORDER_NE], c->w - config.corner, 0, config.corner, config.corner);
	XMoveResizeWindow(dpy, c->curswin[BORDER_SW], 0, c->h - config.corner, config.corner, config.corner);
	XMoveResizeWindow(dpy, c->curswin[BORDER_SE], c->w - config.corner, c->h - config.corner, config.corner, config.corner);
	isshaded = containerisshaded(c);
	TAILQ_FOREACH(col, &c->colq, entry) {
		rowy = c->b;
		if (TAILQ_NEXT(col, entry) != NULL) {
			XMoveResizeWindow(dpy, col->div, col->x + col->w, c->b, config.divwidth, c->h - 2 * c->b);
			XMapWindow(dpy, col->div);
		} else {
			XUnmapWindow(dpy, col->div);
		}
		TAILQ_FOREACH(row, &col->rowq, entry) {
			if (!isshaded) {
				if (TAILQ_NEXT(row, entry) != NULL) {
					XMoveResizeWindow(dpy, row->div, col->x, row->y + row->h, col->w, config.divwidth);
					XMapWindow(dpy, row->div);
				}
				titlebarmoveresize(row, col->x, row->y, col->w);
				if (row->h - config.titlewidth > 0) {
					XMoveResizeWindow(dpy, row->frame, col->x, row->y + config.titlewidth, col->w, row->h - config.titlewidth);
					XMapWindow(dpy, row->frame);
					row->isunmapped = 0;
				} else {
					XUnmapWindow(dpy, row->frame);
					row->isunmapped = 1;
				}
			} else {
				titlebarmoveresize(row, col->x, rowy, col->w);
				XUnmapWindow(dpy, row->frame);
				XUnmapWindow(dpy, row->div);
				row->isunmapped = 1;
			}
			rowy += config.titlewidth;
			TAILQ_FOREACH(t, &row->tabq, entry) {
				tab = (struct Tab *)t;
				XMoveResizeWindow(dpy, tab->frame, 0, 0, tab->winw, tab->winh);
				TAILQ_FOREACH(d, &tab->dialq, entry) {
					dial = (struct Dialog *)d;
					dialogmoveresize(dial);
					ewmhsetframeextents(dial->obj.win, c->b, 0);
				}
				XResizeWindow(dpy, tab->obj.win, tab->winw, tab->winh);
				ewmhsetframeextents(tab->obj.win, c->b, TITLEWIDTH(c));
				tabmoveresize(tab);
			}
		}
	}
	if (!config.disablehidden && checkstack) {
		wm.setclientlist = 1;
	}
}

/* draw decoration on container frame */
void
containerdecorate(struct Container *c, struct Column *cdiv, struct Row *rdiv, int recursive, enum Octant o)
{
	struct Column *col;
	struct Row *row;
	struct Object *t, *d;
	int style;
	int isshaded;

	if (c == NULL)
		return;
	isshaded = containerisshaded(c);
	style = containergetstyle(c);

	/* (re)create pixmap */
	if (c->pw != c->w || c->ph != c->h || c->pix == None)
		pixmapnew(&c->pix, c->frame, c->w, c->h);
	c->pw = c->w;
	c->ph = c->h;

	/* draw background */
	drawbackground(c->pix, 0, 0, c->w, c->h, style);

	if (c->b > 0)
		drawframe(c->pix, isshaded, c->w, c->h, o, style);

	TAILQ_FOREACH(col, &c->colq, entry) {
		/* draw column division */
		if (TAILQ_NEXT(col, entry) != NULL)
			drawshadow(c->pix, col->x + col->w, c->b, config.divwidth, c->h - 2 * c->b, style, col == cdiv);
		TAILQ_FOREACH(row, &col->rowq, entry) {
			/* draw row division */
			if (TAILQ_NEXT(row, entry) != NULL)
				drawshadow(c->pix, col->x, row->y + row->h, col->w, config.divwidth, style, row == rdiv);

			/* (re)create titlebar pixmap */
			if (row->pw != col->w || row->pixbar == None)
				pixmapnew(&row->pixbar, row->bar, col->w, config.titlewidth);
			row->pw = col->w;

			/* draw background of titlebar pixmap */
			drawbackground(row->pixbar, 0, 0, col->w, config.titlewidth, style);
			drawcommit(row->pixbar, row->bar);

			/* draw buttons */
			buttonleftdecorate(row->bl, row->pixbl, style, 0);
			buttonrightdecorate(row->br, row->pixbr, style, 0);

			/* decorate tabs, if necessary */
			if (recursive) {
				TAILQ_FOREACH(t, &row->tabq, entry) {
					tabdecorate((struct Tab *)t, 0);
					TAILQ_FOREACH(d, &((struct Tab *)t)->dialq, entry) {
						dialogdecorate((struct Dialog *)d);
					}
				}
			}
		}
	}

	drawcommit(c->pix, c->frame);
}

/* check if container needs to be redecorated and redecorate it */
void
containerredecorate(struct Container *c, struct Column *cdiv, struct Row *rdiv, enum Octant o)
{
	if (c->pw != c->w || c->ph != c->h) {
		containerdecorate(c, cdiv, rdiv, 0, o);
	}
}

/* calculate position and width of columns of a container */
void
containercalccols(struct Container *c)
{
	struct Column *col;
	int i, x, w;
	int sumw;
	int content;
	int recalc;

	if (c->isfullscreen) {
		c->x = c->mon->mx;
		c->y = c->mon->my;
		c->w = c->mon->mw;
		c->h = c->mon->mh;
		c->b = 0;
		TAILQ_FOREACH(col, &c->colq, entry) {
			col->x = 0;
			col->w = c->w;
			colcalcrows(col, 0);
		}
		return;
	} else if (c->ismaximized) {
		c->x = c->mon->wx;
		c->y = c->mon->wy;
		c->w = c->mon->ww;
		c->h = c->mon->wh;
		c->b = config.borderwidth;
	} else {
		c->x = c->nx;
		c->y = c->ny;
		c->w = c->nw;
		c->h = c->nh;
		c->b = config.borderwidth;
	}
	if (containerisshaded(c)) {
		c->h = 0;
	}

	/* check if columns sum up the width of the container */
	content = containercontentwidth(c);
	sumw = 0;
	recalc = 0;
	TAILQ_FOREACH(col, &c->colq, entry) {
		if (TAILQ_NEXT(col, entry) == NULL) {
			col->w = content - sumw;
		} else {
			col->w = col->fact * content;
		}
		if (col->w == 0) {
			recalc = 1;
		}
		sumw += col->w;
	}
	if (sumw != content)
		recalc = 1;

	w = c->w - 2 * c->b - (c->ncols - 1) * config.divwidth;
	x = c->b;
	i = 0;
	TAILQ_FOREACH(col, &c->colq, entry) {
		if (containerisshaded(c))
			c->h = max(c->h, col->nrows * config.titlewidth);
		if (recalc)
			col->w = max(1, ((i + 1) * w / c->ncols) - (i * w / c->ncols));
		if (recalc)
			col->fact = (double)col->w/(double)c->w;
		col->x = x;
		x += col->w + config.divwidth;
		colcalcrows(col, 0);
		i++;
	}
	if (containerisshaded(c)) {
		c->h += 2 * c->b;
	}
}

/* send container to desktop and focus another on the original desktop */
void
containersendtodeskandfocus(struct Container *c, struct Monitor *mon, unsigned long d)
{
	struct Monitor *prevmon;
	int prevdesk, desk;

	if (c == NULL)
		return;
	prevmon = c->mon;
	prevdesk = c->desk;
	desk = d;

	/* is it necessary to send the container to the desktop */
	if (c->mon != mon || c->desk != desk) {
		/*
		 * Container sent to a desktop which is not the same
		 * as the one it was originally at.
		 */
		if (!containersendtodesk(c, mon, d)) {
			/*
			 * container could not be sent to given desktop;
			 */
			return;
		}
	}

	/* is it necessary to focus something? */
	if (mon == wm.selmon && desk == wm.selmon->seldesk) {
		/*
		 * Container sent to the focused desktop.
		 * Focus it, if visible.
		 */
		if (containerisvisible(c, mon, wm.selmon->seldesk)) {
			tabfocus(c->selcol->selrow->seltab, 0);
		}
	} else if (prevmon == wm.selmon && prevdesk == wm.selmon->seldesk) {
		/*
		 * Container moved from the focused desktop.
		 * Focus the next visible container, if existing;
		 * or nothing, if there's no visible container.
		 */
		if ((c = getnextfocused(mon, prevdesk)) != NULL) {
			tabfocus(c->selcol->selrow->seltab, 0);
		} else {
			tabfocus(NULL, 0);
		}
	}
}

/* move container x pixels to the right and y pixels down */
void
containermove(struct Container *c, int x, int y, int relative)
{
	struct Monitor *monto;
	struct Object *t;
	struct Tab *tab;

	if (c == NULL || c->isminimized || c->ismaximized || c->isfullscreen)
		return;
	if (relative) {
		c->nx += x;
		c->ny += y;
	} else {
		c->nx = x;
		c->ny = y;
	}
	c->x = c->nx;
	c->y = c->ny;
	snaptoedge(&c->x, &c->y, c->w, c->h);
	XMoveWindow(dpy, c->frame, c->x, c->y);
	TAB_FOREACH_BEGIN(c, t){
		tab = (struct Tab *)t;
		winnotify(tab->obj.win, c->x + col->x, c->y + row->y + config.titlewidth, tab->winw, tab->winh);
	}TAB_FOREACH_END
	if (!c->issticky) {
		monto = getmon(c->nx + c->nw / 2, c->ny + c->nh / 2);
		if (monto != NULL && monto != c->mon) {
			c->mon = monto;
			if (wm.focused == c) {
				deskupdate(monto, monto->seldesk);
			}
		}
	}
}

/* raise container */
void
containerraise(struct Container *c, int isfullscreen, int abovebelow)
{
	struct Tab *tab;
	struct Menu *menu;
	struct Object *obj;
	Window wins[2];
	int layer;

	if (c == NULL || c->isminimized)
		return;
	containerdelraise(c);
	wins[1] = c->frame;
	layer = LAYER_NORMAL;
	if (isfullscreen)
		layer = LAYER_FULLSCREEN;
	else if (abovebelow > 0)
		layer = LAYER_ABOVE;
	else if (abovebelow < 0)
		layer = LAYER_BELOW;
	TAILQ_INSERT_AFTER(&wm.stackq, &wm.layers[layer], c, raiseentry);
	wins[0] = wm.layers[layer].frame;
	c->isfullscreen = isfullscreen;
	c->abovebelow = abovebelow;
	XRestackWindows(dpy, wins, 2);
	tab = c->selcol->selrow->seltab;

	/* raise any menu for the container */
	wins[0] = wm.layers[LAYER_MENU].frame;
	TAILQ_FOREACH(obj, &wm.menuq, entry) {
		menu = ((struct Menu *)obj);
		if (!istabformenu(tab, menu))
			continue;
		menu = (struct Menu *)obj;
		wins[1] = menu->frame;
		XRestackWindows(dpy, wins, 2);
		wins[0] = menu->frame;
	}
	wm.setclientlist = 1;
}

/* configure container size and position */
void
containerconfigure(struct Container *c, unsigned int valuemask, XWindowChanges *wc)
{
	if (c == NULL || c->isminimized || c->isfullscreen || c->ismaximized)
		return;
	if (valuemask & CWX)
		c->nx = wc->x;
	if (valuemask & CWY)
		c->ny = wc->y;
	if ((valuemask & CWWidth) && wc->width >= wm.minsize)
		c->nw = wc->width;
	if ((valuemask & CWHeight) && wc->height >= wm.minsize)
		c->nh = wc->height;
	containercalccols(c);
	containermoveresize(c, 1);
	containerredecorate(c, NULL, NULL, 0);
}

/* set container state from client message */
void
containersetstate(struct Tab *tab, Atom *props, unsigned long set)
{
	struct Container *c;

	if (tab == NULL)
		return;
	c = tab->row->col->c;
	if (props[0] == atoms[_NET_WM_STATE_MAXIMIZED_VERT] ||
	    props[0] == atoms[_NET_WM_STATE_MAXIMIZED_HORZ] ||
	    props[1] == atoms[_NET_WM_STATE_MAXIMIZED_VERT] ||
	    props[1] == atoms[_NET_WM_STATE_MAXIMIZED_HORZ])
		containermaximize(c, set);
	if (props[0] == atoms[_NET_WM_STATE_FULLSCREEN] ||
	    props[1] == atoms[_NET_WM_STATE_FULLSCREEN])
		containerfullscreen(c, set);
	if (props[0] == atoms[_NET_WM_STATE_SHADED] ||
	    props[1] == atoms[_NET_WM_STATE_SHADED])
		containershade(c, set);
	if (props[0] == atoms[_NET_WM_STATE_STICKY] ||
	    props[1] == atoms[_NET_WM_STATE_STICKY])
		containerstick(c, set);
	if (props[0] == atoms[_NET_WM_STATE_HIDDEN] ||
	    props[1] == atoms[_NET_WM_STATE_HIDDEN])
		containerminimize(c, set, (c == wm.focused));
	if (props[0] == atoms[_NET_WM_STATE_ABOVE] ||
	    props[1] == atoms[_NET_WM_STATE_ABOVE])
		containerabove(c, set);
	if (props[0] == atoms[_NET_WM_STATE_BELOW] ||
	    props[1] == atoms[_NET_WM_STATE_BELOW])
		containerbelow(c, set);
	if (props[0] == atoms[_NET_WM_STATE_DEMANDS_ATTENTION] ||
	    props[1] == atoms[_NET_WM_STATE_DEMANDS_ATTENTION])
		tabupdateurgency(tab, set == ADD || (set == TOGGLE && !tab->isurgent));
	if (props[0] == atoms[_SHOD_WM_STATE_STRETCHED] ||
	    props[1] == atoms[_SHOD_WM_STATE_STRETCHED]) {
		rowstretch(tab->row->col, tab->row);
		tabfocus(tab->row->seltab, 0);
	}
	ewmhsetstate(c);
}

/* fill placement grid for given rectangle */
static void
fillgrid(struct Monitor *mon, int x, int y, int w, int h, int grid[DIV][DIV])
{
	int i, j;
	int ha, hb, wa, wb;
	int ya, yb, xa, xb;

	for (i = 0; i < DIV; i++) {
		for (j = 0; j < DIV; j++) {
			ha = mon->wy + (mon->wh * i)/DIV;
			hb = mon->wy + (mon->wh * (i + 1))/DIV;
			wa = mon->wx + (mon->ww * j)/DIV;
			wb = mon->wx + (mon->ww * (j + 1))/DIV;
			ya = y;
			yb = y + h;
			xa = x;
			xb = x + w;
			if (ya <= hb && ha <= yb && xa <= wb && wa <= xb) {
				if (ya < ha && yb > hb)
					grid[i][j]++;
				if (xa < wa && xb > wb)
					grid[i][j]++;
				grid[i][j]++;
			}
		}
	}
}

/* find best position to place a container on screen */
void
containerplace(struct Container *c, struct Monitor *mon, int desk, int userplaced)
{
	struct Container *tmp;
	struct Object *obj;
	struct Menu *menu;
	int grid[DIV][DIV] = {{0}, {0}};
	int lowest;
	int i, j, k, w, h;
	int subx, suby;         /* position of the larger subregion */
	int subw, subh;         /* larger subregion width and height */

	if (desk < 0 || desk >= config.ndesktops || c == NULL || c->isminimized)
		return;

	fitmonitor(mon, &c->nx, &c->ny, &c->nw, &c->nh, 1.0);

	/* if the user placed the window, we should not re-place it */
	if (userplaced)
		return;

	/*
	 * The container area is the region of the screen where containers live,
	 * that is, the area of the monitor not occupied by bars or the dock; it
	 * corresponds to the region occupied by a maximized container.
	 *
	 * Shod tries to find an empty region on the container area or a region
	 * with few containers in it to place a new container.  To do that, shod
	 * cuts the container area in DIV divisions horizontally and vertically,
	 * creating DIV*DIV regions; shod then counts how many containers are on
	 * each region; and places the new container on those regions with few
	 * containers over them.
	 *
	 * After some trial and error, I found out that a DIV equals to 15 is
	 * optimal.  It is not too low to provide a incorrect placement, nor too
	 * high to take so much computer time.
	 */

	/* increment cells of grid a window is in */
	TAILQ_FOREACH(tmp, &wm.focusq, entry) {
		if (tmp != c && containerisvisible(tmp, c->mon, c->desk)) {
			fillgrid(mon, tmp->nx, tmp->ny, tmp->nw, tmp->nh, grid);
		}
	}
	TAILQ_FOREACH(obj, &wm.menuq, entry) {
		menu = ((struct Menu *)obj);
		if (istabformenu(c->selcol->selrow->seltab, menu)) {
			fillgrid(mon, menu->x, menu->y, menu->w, menu->h, grid);
		}
	}

	/* find biggest region in grid with less windows in it */
	lowest = INT_MAX;
	subx = suby = 0;
	subw = subh = 0;
	for (i = 0; i < DIV; i++) {
		for (j = 0; j < DIV; j++) {
			if (grid[i][j] > lowest)
				continue;
			else if (grid[i][j] < lowest) {
				lowest = grid[i][j];
				subw = subh = 0;
			}
			for (w = 0; j+w < DIV && grid[i][j + w] == lowest; w++)
				;
			for (h = 1; i+h < DIV && grid[i + h][j] == lowest; h++) {
				for (k = 0; k < w && grid[i + h][j + k] == lowest; k++)
					;
				if (k < w) {
					h--;
					break;
				}
			}
			if (w * h > subw * subh) {
				subw = w;
				subh = h;
				suby = i;
				subx = j;
			}
		}
	}
	subx = subx * mon->ww / DIV;
	suby = suby * mon->wh / DIV;
	subw = subw * mon->ww / DIV;
	subh = subh * mon->wh / DIV;
	c->nx = min(mon->wx + mon->ww - c->nw, max(mon->wx, mon->wx + subx + subw / 2 - c->nw / 2));
	c->ny = min(mon->wy + mon->wh - c->nh, max(mon->wy, mon->wy + suby + subh / 2 - c->nh / 2));
	containercalccols(c);
}

/* check whether container is sticky or is on given desktop */
int
containerisvisible(struct Container *c, struct Monitor *mon, int desk)
{
	return !c->isminimized && c->mon == mon && (c->issticky || c->desk == desk);
}

/* check if container can be shaded */
int
containerisshaded(struct Container *c)
{
	return c->isshaded && !c->isfullscreen;
}

/* attach tab into row; return nonzero if an attachment was performed */
int
tabattach(struct Container *c, struct Tab *det, int x, int y)
{
	enum { CREATTAB = 0x0, CREATROW = 0x1, CREATCOL = 0x2 };
	struct Column *col, *ncol;
	struct Row *row, *nrow;
	struct Tab *tab;
	struct Object *obj;
	int flag;

	if (c == NULL)
		return 0;
	flag = CREATTAB;
	col = NULL;
	row = NULL;
	tab = NULL;
	if (x < config.borderwidth) {
		flag = CREATCOL | CREATROW;
		goto found;
	}
	if (x >= c->w - config.borderwidth) {
		flag = CREATCOL | CREATROW;
		col = TAILQ_LAST(&c->colq, ColumnQueue);
		goto found;
	}
	TAILQ_FOREACH(col, &c->colq, entry) {
		if (TAILQ_NEXT(col, entry) != NULL && x >= col->x + col->w && x < col->x + col->w + config.divwidth) {
			flag = CREATCOL | CREATROW;
			goto found;
		}
		if (x >= col->x && x < col->x + col->w) {
			if (y < config.borderwidth) {
				flag = CREATROW;
				goto found;
			}
			if (y >= c->h - config.borderwidth) {
				flag = CREATROW;
				row = TAILQ_LAST(&col->rowq, RowQueue);
				goto found;
			}
			TAILQ_FOREACH(row, &col->rowq, entry) {
				if (y > row->y && y <= row->y + config.titlewidth) {
					TAILQ_FOREACH_REVERSE(obj, &row->tabq, Queue, entry) {
						tab = (struct Tab *)obj;
						if (x > col->x + tab->x + tab->w / 2) {
							flag = CREATTAB;
							goto found;
						}
					}
					tab = NULL;
					goto found;
				}
				if (TAILQ_NEXT(row, entry) != NULL && y >= row->y + row->h && y < row->y + row->h + config.divwidth) {
					flag = CREATROW;
					goto found;
				}
			}
		}
	}
	return 0;
found:
	ncol = NULL;
	nrow = NULL;
	if (flag & CREATCOL) {
		ncol = colnew();
		containeraddcol(c, ncol, col);
		col = ncol;
	}
	if (flag & CREATROW) {
		nrow = rownew();
		coladdrow(col, nrow, row);
		row = nrow;
	}
	rowaddtab(row, det, tab);
	if (ncol != NULL)
		containercalccols(c);
	else if (nrow != NULL)
		colcalcrows(col, 1);
	else
		rowcalctabs(row);
	tabfocus(det, 0);
	containerraise(c, c->isfullscreen, c->abovebelow);
	XMapSubwindows(dpy, c->frame);
	/* no need to call shodgrouptab() and shodgroupcontainer(); tabfocus() already calls them */
	ewmhsetdesktop(det->obj.win, c->desk);
	containermoveresize(c, 0);
	containerredecorate(c, NULL, NULL, 0);
	return 1;
}

/* delete row, then column if empty, then container if empty */
void
containerdelrow(struct Row *row)
{
	struct Container *c;
	struct Column *col;
	int recalc, redraw;

	col = row->col;
	c = col->c;
	recalc = 1;
	redraw = 0;
	if (row->ntabs == 0) {
		rowdel(row);
		redraw = 1;
	}
	if (col->nrows == 0) {
		coldel(col);
		redraw = 1;
	}
	if (c->ncols == 0) {
		containerdel(c);
		recalc = 0;
	}
	if (recalc) {
		containercalccols(c);
		containermoveresize(c, 0);
		shodgrouptab(c);
		shodgroupcontainer(c);
		if (redraw) {
			containerdecorate(c, NULL, NULL, 0, 0);
		}
	}
}

/* temporarily raise prev/next container and return it; also unhide it if hidden */
struct Container *
containerraisetemp(struct Container *prevc, int backward)
{
	struct Container *newc;
	Window wins[2];

	if (prevc == NULL)
		return NULL;
	if (backward) {
		for (newc = prevc; newc != NULL; newc = TAILQ_PREV(newc, ContainerQueue, entry)) {
			if (newc != prevc &&
			    newc->mon == prevc->mon &&
			    containerisvisible(newc, prevc->mon, prevc->desk)) {
				break;
			}
		}
		if (newc == NULL) {
			TAILQ_FOREACH_REVERSE(newc, &wm.focusq, ContainerQueue, entry) {
				if (newc != prevc &&
				    newc->mon == prevc->mon &&
				    containerisvisible(newc, prevc->mon, prevc->desk)) {
					break;
				}
			}
		}
	} else {
		for (newc = prevc; newc != NULL; newc = TAILQ_NEXT(newc, entry)) {
			if (newc != prevc &&
			    newc->mon == prevc->mon &&
			    containerisvisible(newc, prevc->mon, prevc->desk)) {
				break;
			}
		}
		if (newc == NULL) {
			TAILQ_FOREACH(newc, &wm.focusq, entry) {
				if (newc != prevc &&
				    newc->mon == prevc->mon &&
				    containerisvisible(newc, prevc->mon, prevc->desk)) {
					break;
				}
			}
		}
	}
	if (newc == NULL)
		newc = prevc;
	if (newc->ishidden)
		XMapWindow(dpy, newc->frame);
	/* we save the Z-axis position of the container with wm.restackwin */
	wins[0] = newc->frame;
	wins[1] = wm.restackwin;
	XRestackWindows(dpy, wins, 2);
	XRaiseWindow(dpy, newc->frame);
	wm.focused = newc;
	containerdecorate(newc, NULL, NULL, 1, 0);
	ewmhsetactivewindow(newc->selcol->selrow->seltab->obj.win);
	return newc;
}

/* revert container to its previous position after temporarily raised */
void
containerbacktoplace(struct Container *c, int restack)
{
	Window wins[2];

	if (c == NULL)
		return;
	wm.focused = NULL;
	containerdecorate(c, NULL, NULL, 1, 0);
	if (restack) {
		wins[0] = wm.restackwin;
		wins[1] = c->frame;
		XRestackWindows(dpy, wins, 2);
	}
	if (c->ishidden)
		XUnmapWindow(dpy, c->frame);
	XFlush(dpy);
}

/* detach tab from row, placing it at x,y */
void
tabdetach(struct Tab *tab, int x, int y)
{
	struct Row *row;

	row = tab->row;
	tabremove(row, tab);
	tab->ignoreunmap = IGNOREUNMAP;
	XReparentWindow(dpy, tab->title, root, x, y);
	rowcalctabs(row);
}

/* focus tab */
void
tabfocus(struct Tab *tab, int gotodesk)
{
	struct Container *c;
	struct Dialog *dial;

	wm.prevfocused = wm.focused;
	if (tab == NULL) {
		wm.focused = NULL;
		XSetInputFocus(dpy, wm.focuswin, RevertToParent, CurrentTime);
		ewmhsetactivewindow(None);
	} else {
		c = tab->row->col->c;
		if (!c->isfullscreen && getfullscreen(c->mon, c->desk) != NULL)
			return;         /* we should not focus a client below a fullscreen client */
		wm.focused = c;
		tab->row->seltab = tab;
		tab->row->col->selrow = tab->row;
		tab->row->col->c->selcol = tab->row->col;
		if (gotodesk)
			deskupdate(c->mon, c->issticky ? c->mon->seldesk : c->desk);
		if (tab->row->fact == 0.0)
			rowstretch(tab->row->col, tab->row);
		XRaiseWindow(dpy, tab->row->frame);
		XRaiseWindow(dpy, tab->frame);
		if (c->isshaded || tab->row->isunmapped) {
			XSetInputFocus(dpy, tab->row->bar, RevertToParent, CurrentTime);
		} else if (!TAILQ_EMPTY(&tab->dialq)) {
			dial = (struct Dialog *)TAILQ_FIRST(&tab->dialq);
			XRaiseWindow(dpy, dial->frame);
			XSetInputFocus(dpy, dial->obj.win, RevertToParent, CurrentTime);
		} else {
			XSetInputFocus(dpy, tab->obj.win, RevertToParent, CurrentTime);
		}
		ewmhsetactivewindow(tab->obj.win);
		tabclearurgency(tab);
		containeraddfocus(c);
		containerdecorate(c, NULL, NULL, 1, 0);
		c->isminimized = 0;
		containerhide(c, 0);
		shodgrouptab(c);
		shodgroupcontainer(c);
		ewmhsetstate(c);
	}
	if (wm.prevfocused != NULL && wm.prevfocused != wm.focused) {
		containerdecorate(wm.prevfocused, NULL, NULL, 1, 0);
		ewmhsetstate(wm.prevfocused);
	}
	menuupdate();
}

/* decorate tab */
void
tabdecorate(struct Tab *t, int pressed)
{
	int style;
	int drawlines = 0;

	style = tabgetstyle(t);
	if (t->row != NULL && t != t->row->col->c->selcol->selrow->seltab) {
		pressed = 0;
		drawlines = 0;
	} else if (t->row != NULL && pressed) {
		pressed = 1;
		drawlines = 1;
	} else {
		pressed = 0;
		drawlines = 1;
	}

	/* (re)create pixmap */
	if (t->ptw != t->w || t->pixtitle == None)
		pixmapnew(&t->pixtitle, t->title, t->w, config.titlewidth);
	if (t->pw != t->winw || t->ph != t->winh || t->pix == None)
		pixmapnew(&t->pix, t->frame, t->winw, t->winh);
	t->ptw = t->w;
	t->pw = t->winw;
	t->ph = t->winh;

	/* draw background */
	drawbackground(t->pixtitle, 0, 0, t->w, config.titlewidth, style);

	/* draw shadows */
	drawshadow(t->pixtitle, 0, 0, t->w, config.titlewidth, style, pressed);

	/* write tab title */
	if (t->name != NULL)
		drawtitle(t->pixtitle, t->name, t->w, drawlines, style, pressed, 0);

	/* draw frame background */
	drawbackground(t->pix, 0, 0, t->winw, t->winh, style);

	drawcommit(t->pixtitle, t->title);
	drawcommit(t->pix, t->frame);
}

/* update tab urgency */
void
tabupdateurgency(struct Tab *t, int isurgent)
{
	int prev;

	prev = t->isurgent;
	if (wm.focused != NULL && t == wm.focused->selcol->selrow->seltab)
		t->isurgent = False;
	else
		t->isurgent = isurgent;
	if (prev != t->isurgent) {
		tabdecorate(t, 0);
	}
}

/* stretch row */
void
rowstretch(struct Column *col, struct Row *row)
{
	struct Row *r;
	double fact;
	int refact;

	fact = 1.0 / (double)col->nrows;
	refact = (row->fact == 1.0);
	TAILQ_FOREACH(r, &col->rowq, entry) {
		if (refact) {
			r->fact = fact;
		} else if (r == row) {
			r->fact = 1.0;
		} else {
			r->fact = 0.0;
		}
	}
	colcalcrows(col, 0);
	containermoveresize(col->c, 0);
}

/* configure dialog window */
void
dialogconfigure(struct Dialog *d, unsigned int valuemask, XWindowChanges *wc)
{
	if (d == NULL)
		return;
	if (valuemask & CWWidth)
		d->maxw = wc->width;
	if (valuemask & CWHeight)
		d->maxh = wc->height;
	dialogmoveresize(d);
}

/* commit dialog size and position */
void
dialogmoveresize(struct Dialog *dial)
{
	struct Container *c;
	int dx, dy, dw, dh;

	dialogcalcsize(dial);
	c = dial->tab->row->col->c;
	dx = dial->x - config.borderwidth;
	dy = dial->y - config.borderwidth;
	dw = dial->w + 2 * config.borderwidth;
	dh = dial->h + 2 * config.borderwidth;
	XMoveResizeWindow(dpy, dial->frame, dx, dy, dw, dh);
	XMoveResizeWindow(dpy, dial->obj.win, config.borderwidth, config.borderwidth, dial->w, dial->h);
	winnotify(dial->obj.win, c->x + dial->tab->row->col->x + dial->x, c->y + dial->tab->row->y + dial->y, dial->w, dial->h);
	if (dial->pw != dw || dial->ph != dh) {
		dialogdecorate(dial);
	}
}

/* create container for tab */
void
containernewwithtab(struct Tab *tab, struct Monitor *mon, int desk, XRectangle rect, int state)
{
	struct Container *c;
	struct Column *col;
	struct Row *row;

	if (tab == NULL)
		return;
	c = containernew(rect.x, rect.y, rect.width, rect.height, state);
	c->mon = mon;
	c->desk = desk;
	row = rownew();
	col = colnew();
	containeraddcol(c, col, NULL);
	coladdrow(col, row, NULL);
	rowaddtab(row, tab, NULL);
	containerredecorate(c, NULL, NULL, 0);
	XMapSubwindows(dpy, c->frame);
	containerplace(c, mon, desk, (state & USERPLACED));
	containermoveresize(c, 0);
	if (containerisvisible(c, wm.selmon, wm.selmon->seldesk)) {
		containerraise(c, c->isfullscreen, c->abovebelow);
		containerhide(c, 0);
		tabfocus(tab, 0);
	}
	/* no need to call shodgrouptab() and shodgroupcontainer(); tabfocus() already calls them */
	ewmhsetwmdesktop(c);
	wm.setclientlist = 1;
}

/* create container for tab */
void
managecontainer(struct Tab *prev, struct Monitor *mon, int desk, Window win, Window leader, XRectangle rect, int state, int ignoreunmap)
{
	struct Tab *tab;
	struct Container *c;
	struct Row *row;

	tab = tabnew(win, leader, ignoreunmap);
	winupdatetitle(tab->obj.win, &tab->name);
	if (prev == NULL) {
		containernewwithtab(tab, mon, desk, rect, state);
	} else {
		row = prev->row;
		c = row->col->c;
		rowaddtab(row, tab, prev);
		rowcalctabs(row);
		ewmhsetdesktop(win, c->desk);
		wm.setclientlist = 1;
		containermoveresize(c, 0);
		containerredecorate(c, NULL, NULL, 0);
		XMapSubwindows(dpy, c->frame);
		if (wm.focused == c) {
			tabfocus(tab, 1);
		}
	}
}

/* create container for tab */
void
managedialog(struct Tab *tab, struct Monitor *mon, int desk, Window win, Window leader, XRectangle rect, int state, int ignoreunmap)
{
	struct Dialog *dial;

	(void)mon;
	(void)desk;
	(void)leader;
	(void)state;
	dial = dialognew(win, rect.width, rect.height, ignoreunmap);
	dial->tab = tab;
	TAILQ_INSERT_HEAD(&tab->dialq, (struct Object *)dial, entry);
	XReparentWindow(dpy, dial->frame, tab->frame, 0, 0);
	icccmwmstate(dial->obj.win, NormalState);
	dialogmoveresize(dial);
	XMapRaised(dpy, dial->frame);
	if (wm.focused != NULL && wm.focused->selcol->selrow->seltab == tab)
		tabfocus(tab, 0);
	wm.setclientlist = 1;
}

/* unmanage tab (and delete its row if it is the only tab); return whether deletion occurred */
int
unmanagecontainer(struct Object *obj, int ignoreunmap)
{
	struct Container *c, *next;
	struct Column *col;
	struct Row *row;
	struct Tab *t;
	struct Monitor *mon;
	int desk;
	int moveresize;
	int focus;

	t = (struct Tab *)obj;
	if (ignoreunmap && t->ignoreunmap) {
		t->ignoreunmap--;
		return 0;
	}
	row = t->row;
	col = row->col;
	c = col->c;
	desk = c->desk;
	mon = c->mon;
	moveresize = 1;
	next = c;
	tabdel(t);
	focus = (c == wm.focused);
	if (row->ntabs == 0) {
		rowdel(row);
		if (col->nrows == 0) {
			coldel(col);
			if (c->ncols == 0) {
				containerdel(c);
				next = getnextfocused(mon, desk);
				moveresize = 0;
			}
		}
	}
	if (moveresize) {
		containercalccols(c);
		containermoveresize(c, 0);
		containerredecorate(c, NULL, NULL, 0);
		shodgrouptab(c);
		shodgroupcontainer(c);
	}
	if (focus) {
		tabfocus((next != NULL) ? next->selcol->selrow->seltab : NULL, 0);
	}
	return 1;
}

/* delete dialog; return whether dialog was deleted */
int
unmanagedialog(struct Object *obj, int ignoreunmap)
{
	struct Dialog *dial;

	dial = (struct Dialog *)obj;
	if (ignoreunmap && dial->ignoreunmap) {
		dial->ignoreunmap--;
		return 0;
	}
	TAILQ_REMOVE(&dial->tab->dialq, (struct Object *)dial, entry);
	if (dial->pix != None)
		XFreePixmap(dpy, dial->pix);
	icccmdeletestate(dial->obj.win);
	XReparentWindow(dpy, dial->obj.win, root, 0, 0);
	XDestroyWindow(dpy, dial->frame);
	free(dial);
	return 1;
}

/* get height of column without borders, divisors, title bars, etc */
int
columncontentheight(struct Column *col)
{
	return col->c->h - col->nrows * config.titlewidth
	       - (col->nrows - 1) * config.divwidth - 2 * col->c->b;
}

/* get width of container without borders, divisors, etc */
int
containercontentwidth(struct Container *c)
{
	return c->w - (c->ncols - 1) * config.divwidth - 2 * c->b;
}
