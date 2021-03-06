/*
 *  This file is part of the Jikes RVM project (http://jikesrvm.org).
 *
 *  This file is licensed to You under the Eclipse Public License (EPL);
 *  You may not use this file except in compliance with the License. You
 *  may obtain a copy of the License at
 *
 *      http://www.opensource.org/licenses/eclipse-1.0.php
 *
 *  See the COPYRIGHT.txt file distributed with this work for information
 *  regarding copyright ownership.
 */
package org.jikesrvm.mm.mmtk.gcspy;

import static org.jikesrvm.runtime.SysCall.sysCall;
import static org.mmtk.utility.gcspy.StreamConstants.INT_TYPE;

import org.jikesrvm.VM;
import org.mmtk.utility.gcspy.Color;
import org.mmtk.utility.gcspy.GCspy;
import org.mmtk.utility.gcspy.drivers.AbstractDriver;
import org.vmmagic.pragma.Uninterruptible;
import org.vmmagic.unboxed.Address;

/**
 * Set up a GCspy Stream with data type INT_TYPE.
 */

@Uninterruptible public class IntStream extends org.mmtk.vm.gcspy.IntStream {

  /****************************************************************************
   *
   * Initialization
   */

  /**
   * Construct a new GCspy stream of INT_TYPE.
   * @param driver         The driver that owns this Stream
   * @param name           The name of the stream (e.g. "Used space")
   * @param minValue       The minimum value for any item in this stream.
   *                       Values less than this will be represented as "minValue-"
   * @param maxValue       The maximum value for any item in this stream.
   *                       Values greater than this will be represented as "maxValue+"
   * @param zeroValue      The zero value for this stream
   * @param defaultValue   The default value for this stream
   * @param stringPre      A string to prefix values (e.g. "Used: ")
   * @param stringPost     A string to suffix values (e.g. " bytes.")
   * @param presentation   How a stream value is to be presented.
   * @param paintStyle     How the value is to be painted.
   * @param indexMaxStream The index of the maximum stream if the presentation is *_VAR.
   * @param colour         The default colour for tiles of this stream
   * @param summary        Is a summary enabled?
   */
  public IntStream(
         AbstractDriver driver,
         String name,
         int minValue,
         int maxValue,
         int zeroValue,
         int defaultValue,
         String stringPre,
         String stringPost,
         int presentation,
         int paintStyle,
         int indexMaxStream,
         Color colour,
         boolean summary) {

    super(driver, name,
          minValue, maxValue, zeroValue, defaultValue,
          stringPre, stringPost, presentation, paintStyle,
          indexMaxStream, colour, summary);

    if (VM.BuildWithGCSpy) {
      // We never delete these
      Address tmpName = GCspy.util.getBytes(name);
      Address tmpPre = GCspy.util.getBytes(stringPre);
      Address tmpPost = GCspy.util.getBytes(stringPost);

      sysCall.gcspyStreamInit(stream, streamId, INT_TYPE,
          tmpName, minValue, maxValue, zeroValue, defaultValue,
          tmpPre, tmpPost, presentation, paintStyle, indexMaxStream,
          colour.getRed(), colour.getGreen(), colour.getBlue());
    }
  }

}

