/**
 * Paints the tile overlay onto a canvas laid over the terminal.
 *
 * Kept free of DOM lookups and xterm internals so it can be unit tested: the
 * caller measures the terminal and hands over the geometry.
 */

import { tileRect, type GlyphFlags, type TilesetManifest } from "./protocol";

/** Everything the painter needs to know about where to draw. */
export interface OverlayTarget {
  /** The loaded tile sheet. */
  sheet: CanvasImageSource;
  manifest: TilesetManifest;
  /** Size of one terminal cell, in CSS pixels. */
  cellWidth: number;
  cellHeight: number;
  /** Size of the overlay canvas, in CSS pixels. */
  widthPx: number;
  heightPx: number;
  /**
   * Draw each tile at a whole-number multiple of its native pixel size,
   * centred in its cell, instead of stretching it to fill. Sharper, but it
   * needs a cell at least as large as one tile -- which is what the cell
   * width and height controls are for.
   */
  pixelPerfect?: boolean;
  /**
   * Device pixels per CSS pixel (the canvas transform's scale). Cell edges are
   * snapped to whole device pixels so neighbouring tiles meet exactly; without
   * it a fractional cell size leaves each tile edge part-covering a pixel,
   * which the canvas anti-aliases into a thin seam of the terminal behind.
   */
  pixelRatio?: number;
}

export interface OverlayTile {
  row: number;
  col: number;
  tile: number;
  flags: GlyphFlags;
  ch: string;
}

/** What a paint actually managed to draw, so the UI can spot a bad sheet. */
export interface PaintReport {
  drawn: number;
  /** Tiles whose index is not in the sheet -- the version-mismatch signal. */
  missing: number;
  /** Highest index the server asked for, or -1 if none. */
  maxIndex: number;
}

/** Where the terminal's own cursor is, in cells. */
export interface CursorCell {
  row: number;
  col: number;
}

/** Matches the terminal theme's cursor colour. */
const CURSOR_COLOR = "#ffb642";

export function paintOverlay(
  ctx: CanvasRenderingContext2D,
  target: OverlayTarget,
  tiles: readonly OverlayTile[],
  cursor?: CursorCell,
): PaintReport {
  const report: PaintReport = { drawn: 0, missing: 0, maxIndex: -1 };
  const { manifest, cellWidth, cellHeight, widthPx, heightPx } = target;
  const ratio = target.pixelRatio && target.pixelRatio > 0 ? target.pixelRatio : 1;
  /** Rounds a CSS-pixel coordinate to the nearest device pixel boundary. */
  const snap = (v: number) => Math.round(v * ratio) / ratio;
  /** A cell's box with every edge on a device pixel, shared with its neighbours. */
  const cellBox = (row: number, col: number) => {
    const left = snap(col * cellWidth);
    const top = snap(row * cellHeight);
    return {
      x: left,
      y: top,
      w: snap((col + 1) * cellWidth) - left,
      h: snap((row + 1) * cellHeight) - top,
    };
  };
  let cursorCovered = false;

  ctx.clearRect(0, 0, widthPx, heightPx);
  // NetHack tiles are pixel art; smoothing turns them to mush when the cell
  // size does not divide evenly by the tile size.
  ctx.imageSmoothingEnabled = false;

  for (const tile of tiles) {
    report.maxIndex = Math.max(report.maxIndex, tile.tile);
    const { x, y, w, h } = cellBox(tile.row, tile.col);
    if (x >= widthPx || y >= heightPx || x + w <= 0 || y + h <= 0) {
      continue;
    }

    const src = tileRect(manifest, tile.tile);
    if (!src) {
      report.missing++;
      drawMissingTile(ctx, x, y, w, h);
      continue;
    }
    report.drawn++;

    // How many whole tiles fit in the cell; 0 means the cell is too small for
    // even one, so stretching is the only option that stays inside the cell.
    const whole = target.pixelPerfect
      ? Math.floor(Math.min(w / src.width, h / src.height))
      : 0;
    const drawWidth = whole >= 1 ? src.width * whole : w;
    const drawHeight = whole >= 1 ? src.height * whole : h;

    ctx.drawImage(
      target.sheet,
      src.x,
      src.y,
      src.width,
      src.height,
      x + snap((w - drawWidth) / 2),
      y + snap((h - drawHeight) / 2),
      drawWidth,
      drawHeight,
    );

    if (tile.flags.detected || tile.flags.invisible) {
      ctx.fillStyle = tile.flags.invisible
        ? "rgba(255, 255, 255, 0.25)"
        : "rgba(120, 180, 255, 0.30)";
      ctx.fillRect(x, y, w, h);
    }

    if (tile.flags.pet) {
      drawPetHeart(ctx, x, y, w, h);
    }

    if (cursor && cursor.row === tile.row && cursor.col === tile.col) {
      cursorCovered = true;
    }
  }

  if (cursor && cursorCovered) {
    const box = cellBox(cursor.row, cursor.col);
    drawCursor(ctx, box.x, box.y, box.w, box.h);
  }

  return report;
}

/**
 * Rings the cell the terminal's cursor is in.
 *
 * Only called when a tile is covering that cell. The overlay canvas sits on
 * top of the terminal, so xterm's cursor block is behind the tile and
 * invisible -- and during travel or a `;` look that cursor *is* the thing the
 * player is aiming, which made choosing a destination impossible on any
 * square that had a tile.
 */
function drawCursor(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  // An outline rather than a filled block: the tile underneath is what the
  // player is aiming at, so it has to stay visible.
  ctx.strokeStyle = CURSOR_COLOR;
  ctx.lineWidth = 2;
  ctx.strokeRect(x + 1, y + 1, w - 2, h - 2);
  ctx.lineWidth = 1;
}

/**
 * A tile index the sheet does not cover means the tileset does not match the
 * server's NetHack version. Draw something obvious rather than a hole.
 */
function drawMissingTile(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  ctx.fillStyle = "#3a1020";
  ctx.fillRect(x, y, w, h);
  ctx.strokeStyle = "#ff5577";
  ctx.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1);
  ctx.fillStyle = "#ff99aa";
  ctx.font = `${Math.floor(h * 0.7)}px monospace`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText("?", x + w / 2, y + h / 2);
}

/** The classic tty pet marker, drawn in the corner of the tile. */
function drawPetHeart(
  ctx: CanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
): void {
  const size = Math.max(3, Math.min(w, h) * 0.35);
  const cx = x + size * 0.7;
  const cy = y + h - size * 0.7;

  ctx.fillStyle = "#ff3355";
  ctx.beginPath();
  ctx.moveTo(cx, cy + size * 0.35);
  ctx.lineTo(cx - size * 0.5, cy - size * 0.1);
  ctx.lineTo(cx - size * 0.25, cy - size * 0.45);
  ctx.lineTo(cx, cy - size * 0.15);
  ctx.lineTo(cx + size * 0.25, cy - size * 0.45);
  ctx.lineTo(cx + size * 0.5, cy - size * 0.1);
  ctx.closePath();
  ctx.fill();
}
